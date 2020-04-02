// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "primitives/transaction.h"

#include "hash.h"
#include "tinyformat.h"
#include "utilstrencodings.h"
#include "librustzcash.h"
#include <boost/foreach.hpp>

// static global check methods, now called by CTransaction instances
#include "main.h"
#include "sc/sidechain.h"
#include "sc/sidechainrpc.h"
#include "consensus/validation.h"
#include "validationinterface.h"
#include "undo.h"
#include "core_io.h"
#include "miner.h"
#include "utilmoneystr.h"
#include <univalue.h>

extern UniValue TxJoinSplitToJSON(const CTransaction& tx);

JSDescription JSDescription::getNewInstance(bool useGroth) {
	JSDescription js;

	if(useGroth) {
		js.proof = libzcash::GrothProof();
	} else {
		js.proof = libzcash::PHGRProof();
	}

	return js;
}

JSDescription::JSDescription(
    bool makeGrothProof,
    ZCJoinSplit& params,
    const uint256& joinSplitPubKey,
    const uint256& anchor,
    const std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS>& inputs,
    const std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS>& outputs,
    CAmount vpub_old,
    CAmount vpub_new,
    bool computeProof,
    uint256 *esk // payment disclosure
) : vpub_old(vpub_old), vpub_new(vpub_new), anchor(anchor)
{
    std::array<libzcash::Note, ZC_NUM_JS_OUTPUTS> notes;

    proof = params.prove(
        makeGrothProof,
        inputs,
        outputs,
        notes,
        ciphertexts,
        ephemeralKey,
        joinSplitPubKey,
        randomSeed,
        macs,
        nullifiers,
        commitments,
        vpub_old,
        vpub_new,
        anchor,
        computeProof,
        esk // payment disclosure
    );
}

JSDescription JSDescription::Randomized(
    bool makeGrothProof,
    ZCJoinSplit& params,
    const uint256& joinSplitPubKey,
    const uint256& anchor,
    std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS>& inputs,
    std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS>& outputs,
    #ifdef __LP64__ // required to build on MacOS due to size_t ambiguity errors
    std::array<uint64_t, ZC_NUM_JS_INPUTS>& inputMap,
    std::array<uint64_t, ZC_NUM_JS_OUTPUTS>& outputMap,
    #else
    std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
    std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap,
    #endif
    
    CAmount vpub_old,
    CAmount vpub_new,
    bool computeProof,
    uint256 *esk, // payment disclosure
    std::function<int(int)> gen
)
{
    // Randomize the order of the inputs and outputs
    inputMap = {0, 1};
    outputMap = {0, 1};

    assert(gen);

    MappedShuffle(inputs.begin(), inputMap.begin(), ZC_NUM_JS_INPUTS, gen);
    MappedShuffle(outputs.begin(), outputMap.begin(), ZC_NUM_JS_OUTPUTS, gen);

    return JSDescription(
        makeGrothProof,
        params, joinSplitPubKey, anchor, inputs, outputs,
        vpub_old, vpub_new, computeProof,
        esk // payment disclosure
    );
}

class SproutProofVerifier : public boost::static_visitor<bool>
{
    ZCJoinSplit& params;
    libzcash::ProofVerifier& verifier;
    const uint256& joinSplitPubKey;
    const JSDescription& jsdesc;

public:
    SproutProofVerifier(
        ZCJoinSplit& params,
        libzcash::ProofVerifier& verifier,
        const uint256& joinSplitPubKey,
        const JSDescription& jsdesc
        ) : params(params), verifier(verifier), joinSplitPubKey(joinSplitPubKey), jsdesc(jsdesc) {}

    bool operator()(const libzcash::PHGRProof& proof) const
    {
        return params.verify(
            proof,
            verifier,
            joinSplitPubKey,
            jsdesc.randomSeed,
            jsdesc.macs,
            jsdesc.nullifiers,
            jsdesc.commitments,
            jsdesc.vpub_old,
            jsdesc.vpub_new,
            jsdesc.anchor
        );
    }

    bool operator()(const libzcash::GrothProof& proof) const
    {
        uint256 h_sig = params.h_sig(jsdesc.randomSeed, jsdesc.nullifiers, joinSplitPubKey);

        return librustzcash_sprout_verify(
            proof.begin(),
            jsdesc.anchor.begin(),
            h_sig.begin(),
            jsdesc.macs[0].begin(),
            jsdesc.macs[1].begin(),
            jsdesc.nullifiers[0].begin(),
            jsdesc.nullifiers[1].begin(),
            jsdesc.commitments[0].begin(),
            jsdesc.commitments[1].begin(),
            jsdesc.vpub_old,
            jsdesc.vpub_new
        );
    }
};

bool JSDescription::Verify(
    ZCJoinSplit& params,
    libzcash::ProofVerifier& verifier,
    const uint256& joinSplitPubKey
) const {
    auto pv = SproutProofVerifier(params, verifier, joinSplitPubKey, *this);
    return boost::apply_visitor(pv, proof);
}

uint256 JSDescription::h_sig(ZCJoinSplit& params, const uint256& joinSplitPubKey) const
{
    return params.h_sig(randomSeed, nullifiers, joinSplitPubKey);
}

std::string COutPoint::ToString() const
{
    return strprintf("COutPoint(%s, %u)", hash.ToString().substr(0,10), n);
}

CTxIn::CTxIn(const COutPoint& prevoutIn, const CScript& scriptSigIn, uint32_t nSequenceIn)
{
    prevout = prevoutIn;
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

CTxIn::CTxIn(const uint256& hashPrevTx, const uint32_t& nOut, const CScript& scriptSigIn, uint32_t nSequenceIn)
{
    prevout = COutPoint(hashPrevTx, nOut);
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

std::string CTxIn::ToString() const
{
    std::string str;
    str += "CTxIn(";
    str += prevout.ToString();
    if (prevout.IsNull())
        str += strprintf(", coinbase %s", HexStr(scriptSig));
    else
        str += strprintf(", scriptSig=%s", HexStr(scriptSig).substr(0, 24));
    if (nSequence != std::numeric_limits<unsigned int>::max())
        str += strprintf(", nSequence=%u", nSequence);
    str += ")";
    return str;
}

CBackwardTransferOut::CBackwardTransferOut(const CTxOut& txout): nValue(txout.nValue)
{
    auto it = std::find(txout.scriptPubKey.begin(), txout.scriptPubKey.end(), OP_HASH160);
    assert(it != txout.scriptPubKey.end());
    ++it; 
    assert(*it == sizeof(uint160));
    ++it;
    std::vector<unsigned char>  pubKeyV(it, (it + sizeof(uint160)));
    pubKeyHash = uint160(pubKeyV);
}

CTxOut::CTxOut(const CBackwardTransferOut& btout) : nValue(btout.nValue)
{
    scriptPubKey.clear();
    std::vector<unsigned char> pkh(btout.pubKeyHash.begin(), btout.pubKeyHash.end());
    scriptPubKey << OP_DUP << OP_HASH160 << pkh << OP_EQUALVERIFY << OP_CHECKSIG;
    isFromBackwardTransfer = true;
}

uint256 CTxOut::GetHash() const
{
    return SerializeHash(*this);
}

std::string CTxOut::ToString() const
{
    return strprintf("CTxOut(nValue=%d.%08d, scriptPubKey=%s)", nValue / COIN, nValue % COIN, HexStr(scriptPubKey).substr(0, 30));
}

//----------------------------------------------------------------------------
uint256 CTxForwardTransferOut::GetHash() const
{
    return SerializeHash(*this);
}

std::string CTxForwardTransferOut::ToString() const
{
    return strprintf("CTxForwardTransferOut(nValue=%d.%08d, address=%s, scId=%s)",
        nValue / COIN, nValue % COIN, HexStr(address).substr(0, 30), scId.ToString() );
}

//----------------------------------------------------------------------------
uint256 CTxCertifierLockOut::GetHash() const
{
    return SerializeHash(*this);
}

std::string CTxCertifierLockOut::ToString() const
{
    return strprintf("CTxCertifierLockOut(nValue=%d.%08d, address=%s, scId=%s, activeFromWithdrawalEpoch=%lld",
        nValue / COIN, nValue % COIN, HexStr(address).substr(0, 30), scId.ToString(), activeFromWithdrawalEpoch);
}

//----------------------------------------------------------------------------
bool CTxCrosschainOut::CheckAmountRange(CAmount& cumulatedAmount) const
{
    if (nValue == CAmount(0) || !MoneyRange(nValue))
    {
        LogPrint("sc", "%s():%d - ERROR: invalid nValue %lld\n", __func__, __LINE__, nValue);
        return false;
    }

    cumulatedAmount += nValue;

    if (!MoneyRange(cumulatedAmount))
    {
        LogPrint("sc", "%s():%d - ERROR: invalid cumulated value %lld\n", __func__, __LINE__, cumulatedAmount);
        return false;
    }

    return true;
}

CTxScCreationOut::CTxScCreationOut(
    const uint256& scIdIn, const CAmount& nValueIn, const uint256& addressIn,
    const Sidechain::ScCreationParameters& paramsIn)
    :CTxCrosschainOut(scIdIn, nValueIn, addressIn),
     withdrawalEpochLength(paramsIn.withdrawalEpochLength), customData(paramsIn.customData) {}

uint256 CTxScCreationOut::GetHash() const
{
    return SerializeHash(*this);
}

std::string CTxScCreationOut::ToString() const
{
    return strprintf("CTxScCreationOut(scId=%s, withdrawalEpochLength=%d, nValue=%d.%08d, address=%s, customData=[%s]",
        scId.ToString(), withdrawalEpochLength, nValue / COIN, nValue % COIN, HexStr(address).substr(0, 30), HexStr(customData) );
}


CMutableTransactionBase::CMutableTransactionBase() :
    nVersion(TRANSPARENT_TX_VERSION), vout() {}

CMutableTransaction::CMutableTransaction() : CMutableTransactionBase(), nLockTime(0) {}

CMutableTransaction::CMutableTransaction(const CTransaction& tx) :
    vsc_ccout(tx.vsc_ccout), vcl_ccout(tx.vcl_ccout), vft_ccout(tx.vft_ccout), nLockTime(tx.nLockTime),
    vjoinsplit(tx.GetVjoinsplit()), joinSplitPubKey(tx.joinSplitPubKey), joinSplitSig(tx.joinSplitSig)
{
    nVersion = tx.nVersion;
    vin = tx.GetVin();
    vout = tx.GetVout();
}
    
uint256 CMutableTransaction::GetHash() const
{
    return SerializeHash(*this);
}

bool CMutableTransaction::add(const CTxScCreationOut& out) 
{
    vsc_ccout.push_back(out);
    return true;
}

bool CMutableTransaction::add(const CTxCertifierLockOut& out) 
{
    vcl_ccout.push_back(out);
    return true;
}

bool CMutableTransaction::add(const CTxForwardTransferOut& out)
{
    vft_ccout.push_back(out);
    return true;
}

//--------------------------------------------------------------------------------------------------------
CTransactionBase::CTransactionBase() :
    nVersion(TRANSPARENT_TX_VERSION), vout() {}

CTransactionBase& CTransactionBase::operator=(const CTransactionBase &tx) {
    *const_cast<uint256*>(&hash) = tx.hash;
    *const_cast<int*>(&nVersion) = tx.nVersion;
    *const_cast<std::vector<CTxOut>*>(&vout) = tx.vout;
    return *this;
}

CTransactionBase::CTransactionBase(const CTransactionBase &tx) : nVersion(TRANSPARENT_TX_VERSION) {
    *const_cast<uint256*>(&hash) = tx.hash;
    *const_cast<int*>(&nVersion) = tx.nVersion;
    *const_cast<std::vector<CTxOut>*>(&vout) = tx.vout;
}

CAmount CTransactionBase::GetValueOut() const
{
    CAmount nValueOut = 0;
    for (std::vector<CTxOut>::const_iterator it(vout.begin()); it != vout.end(); ++it)
    {
        nValueOut += it->nValue;
        if (!MoneyRange(it->nValue) || !MoneyRange(nValueOut))
            throw std::runtime_error("CTransactionBase::GetValueOut(): value out of range");
    }
    return nValueOut;
}

bool CTransactionBase::CheckInputsAmount(CValidationState &state) const
{
    // Ensure input values do not exceed MAX_MONEY
    // We have not resolved the txin values at this stage,
    // but we do know what the joinsplits claim to add
    // to the value pool.
    CAmount nCumulatedValueIn = 0;
    for (std::vector<JSDescription>::const_iterator it(GetVjoinsplit().begin()); it != GetVjoinsplit().end(); ++it)
    {
        nCumulatedValueIn += it->vpub_new;

        if (!MoneyRange(it->vpub_new) || !MoneyRange(nCumulatedValueIn)) {
            return state.DoS(100, error("CheckTransaction(): txin total out of range"),
                             REJECT_INVALID, "bad-txns-txintotal-toolarge");
        }
    }

    return true;
}

bool CTransactionBase::CheckOutputsAmount(CValidationState &state) const
{
    // Check for negative or overflow output values
    CAmount nCumulatedValueOut = 0;
    for(const CTxOut& txout: GetVout())
    {
        if (txout.nValue < 0)
            return state.DoS(100, error("CheckOutputAmounts(): txout.nValue negative"),
                             REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, error("CheckOutputAmounts(): txout.nValue too high"),
                             REJECT_INVALID, "bad-txns-vout-toolarge");
        nCumulatedValueOut += txout.nValue;
        if (!MoneyRange(nCumulatedValueOut))
            return state.DoS(100, error("CheckOutputAmounts(): txout total out of range"),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Ensure that joinsplit values are well-formed
    for(const JSDescription& joinsplit: GetVjoinsplit())
    {
        if (joinsplit.vpub_old < 0) {
            return state.DoS(100, error("CheckOutputAmounts(): joinsplit.vpub_old negative"),
                             REJECT_INVALID, "bad-txns-vpub_old-negative");
        }

        if (joinsplit.vpub_new < 0) {
            return state.DoS(100, error("CheckOutputAmounts(): joinsplit.vpub_new negative"),
                             REJECT_INVALID, "bad-txns-vpub_new-negative");
        }

        if (joinsplit.vpub_old > MAX_MONEY) {
            return state.DoS(100, error("CheckOutputAmounts(): joinsplit.vpub_old too high"),
                             REJECT_INVALID, "bad-txns-vpub_old-toolarge");
        }

        if (joinsplit.vpub_new > MAX_MONEY) {
            return state.DoS(100, error("CheckOutputAmounts(): joinsplit.vpub_new too high"),
                             REJECT_INVALID, "bad-txns-vpub_new-toolarge");
        }

        if (joinsplit.vpub_new != 0 && joinsplit.vpub_old != 0) {
            return state.DoS(100, error("CheckOutputAmounts(): joinsplit.vpub_new and joinsplit.vpub_old both nonzero"),
                             REJECT_INVALID, "bad-txns-vpubs-both-nonzero");
        }

        nCumulatedValueOut += joinsplit.vpub_old;
        if (!MoneyRange(nCumulatedValueOut)) {
            return state.DoS(100, error("CheckOutputAmounts(): txout total out of range"),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
        }
    }

    return true;
}

bool CTransactionBase::CheckInputsDuplication(CValidationState &state) const
{
    // Check for duplicate inputs
    std::set<COutPoint> vInOutPoints;
    for(const CTxIn& txin: GetVin())
    {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("CheckInputsDuplications(): duplicate inputs"),
                             REJECT_INVALID, "bad-txns-inputs-duplicate");
        vInOutPoints.insert(txin.prevout);
    }

    // Check for duplicate joinsplit nullifiers in this transaction
    std::set<uint256> vJoinSplitNullifiers;
    for(const JSDescription& joinsplit: GetVjoinsplit())
    {
        for(const uint256& nf: joinsplit.nullifiers)
        {
            if (vJoinSplitNullifiers.count(nf))
                return state.DoS(100, error("CheckInputsDuplications(): duplicate nullifiers"),
                             REJECT_INVALID, "bad-joinsplits-nullifiers-duplicate");

            vJoinSplitNullifiers.insert(nf);
        }
    }

    return true;
}

bool CTransactionBase::CheckInputsInteraction(CValidationState &state) const
{
    if (IsCoinBase())
    {
        // There should be no joinsplits in a coinbase transaction
        if (GetVjoinsplit().size() > 0)
            return state.DoS(100, error("CheckInputsInteraction(): coinbase has joinsplits"),
                             REJECT_INVALID, "bad-cb-has-joinsplits");

        if (GetVin()[0].scriptSig.size() < 2 || GetVin()[0].scriptSig.size() > 100)
            return state.DoS(100, error("CheckInputsInteraction(): coinbase script size"),
                             REJECT_INVALID, "bad-cb-length");
    }
    else
    {
        for(const CTxIn& txin: GetVin())
            if (txin.prevout.IsNull())
                return state.DoS(10, error("CheckInputsInteraction(): prevout is null"),
                                 REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}

CTransaction::CTransaction() :
    CTransactionBase(), vin(),
    vsc_ccout(), vcl_ccout(), vft_ccout(),
    nLockTime(0), vjoinsplit(), joinSplitPubKey(), joinSplitSig() { }

void CTransaction::UpdateHash() const
{
    *const_cast<uint256*>(&hash) = SerializeHash(*this);
}

CTransaction::CTransaction(const CMutableTransaction &tx) :
    vin(tx.vin), vsc_ccout(tx.vsc_ccout), vcl_ccout(tx.vcl_ccout), vft_ccout(tx.vft_ccout),
    nLockTime(tx.nLockTime), vjoinsplit(tx.vjoinsplit), joinSplitPubKey(tx.joinSplitPubKey), joinSplitSig(tx.joinSplitSig)
{
    *const_cast<int*>(&nVersion) = tx.nVersion;
    *const_cast<std::vector<CTxOut>*>(&vout) = tx.vout;
    UpdateHash();
}

CTransaction& CTransaction::operator=(const CTransaction &tx) {
    CTransactionBase::operator=(tx);
    *const_cast<std::vector<CTxIn>*>(&vin) = tx.vin;
    *const_cast<std::vector<CTxScCreationOut>*>(&vsc_ccout) = tx.vsc_ccout;
    *const_cast<std::vector<CTxCertifierLockOut>*>(&vcl_ccout) = tx.vcl_ccout;
    *const_cast<std::vector<CTxForwardTransferOut>*>(&vft_ccout) = tx.vft_ccout;
    *const_cast<uint32_t*>(&nLockTime) = tx.nLockTime;
    *const_cast<std::vector<JSDescription>*>(&vjoinsplit) = tx.vjoinsplit;
    *const_cast<uint256*>(&joinSplitPubKey) = tx.joinSplitPubKey;
    *const_cast<joinsplit_sig_t*>(&joinSplitSig) = tx.joinSplitSig;
    return *this;
}

CTransaction::CTransaction(const CTransaction &tx) : nLockTime(0)
{
    // call explicitly the copy of members of virtual base class
    *const_cast<uint256*>(&hash) = tx.hash;
    *const_cast<int*>(&nVersion) = tx.nVersion;
    *const_cast<std::vector<CTxOut>*>(&vout) = tx.vout;
    //---
    *const_cast<std::vector<CTxIn>*>(&vin) = tx.vin;
    *const_cast<std::vector<CTxScCreationOut>*>(&vsc_ccout) = tx.vsc_ccout;
    *const_cast<std::vector<CTxCertifierLockOut>*>(&vcl_ccout) = tx.vcl_ccout;
    *const_cast<std::vector<CTxForwardTransferOut>*>(&vft_ccout) = tx.vft_ccout;
    *const_cast<uint32_t*>(&nLockTime) = tx.nLockTime;
    *const_cast<std::vector<JSDescription>*>(&vjoinsplit) = tx.vjoinsplit;
    *const_cast<uint256*>(&joinSplitPubKey) = tx.joinSplitPubKey;
    *const_cast<joinsplit_sig_t*>(&joinSplitSig) = tx.joinSplitSig;
}

unsigned int CTransaction::CalculateModifiedSize(unsigned int nTxSize) const
{
    // In order to avoid disincentivizing cleaning up the UTXO set we don't count
    // the constant overhead for each txin and up to 110 bytes of scriptSig (which
    // is enough to cover a compressed pubkey p2sh redemption) for priority.
    // Providing any more cleanup incentive than making additional inputs free would
    // risk encouraging people to create junk outputs to redeem later.
    if (nTxSize == 0)
    {
        // polymorphic call
        nTxSize = CalculateSize();
    }
    for (std::vector<CTxIn>::const_iterator it(vin.begin()); it != vin.end(); ++it)
    {
        unsigned int offset = 41U + std::min(110U, (unsigned int)it->scriptSig.size());
        if (nTxSize > offset)
            nTxSize -= offset;
    }
    return nTxSize;
}

double CTransaction::ComputePriority(double dPriorityInputs, unsigned int nTxSize) const
{
    nTxSize = CalculateModifiedSize(nTxSize);
    if (nTxSize == 0) return 0.0;

    return dPriorityInputs / nTxSize;
}

bool CTransaction::CheckVersionBasic(CValidationState &state) const
{
    // Basic checks that don't depend on any context
    // Check transaction version
    if (nVersion < MIN_OLD_TX_VERSION && nVersion != GROTH_TX_VERSION && !IsScVersion() )
    {
        return state.DoS(100, error("BasicVersionCheck(): version too low"),
                         REJECT_INVALID, "bad-txns-version-too-low");
    }

    return true;
}

bool CTransaction::CheckInputsAvailability(CValidationState &state) const
{
    // Transactions can contain empty `vin` and `vout` so long as
    // `vjoinsplit` is non-empty.
    if (GetVin().empty() && GetVjoinsplit().empty())
    {
        LogPrint("sc", "%s():%d - Error: tx[%s]\n", __func__, __LINE__, GetHash().ToString() );
        return state.DoS(10, error("CheckInputsAvailability(): vin empty"),
                         REJECT_INVALID, "bad-txns-vin-empty");
    }

    return true;
}

bool CTransaction::CheckSerializedSize(CValidationState &state) const
{
    BOOST_STATIC_ASSERT(MAX_BLOCK_SIZE > MAX_TX_SIZE); // sanity
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_TX_SIZE)
        return state.DoS(100, error("checkSerializedSizeLimits(): size limits failed"),
                         REJECT_INVALID, "bad-txns-oversize");

    return true;
}

bool CTransaction::CheckOutputsAvailability(CValidationState &state) const
{
    // Allow the case when crosschain outputs are not empty. In that case there might be no vout at all
    // when utxo reminder is only dust, which is added to fee leaving no change for the sender
    if (GetVout().empty() && GetVjoinsplit().empty() && ccIsNull())
    {
        return state.DoS(10, error("CheckOutputsAvailability(): vout empty"),
                         REJECT_INVALID, "bad-txns-vout-empty");
    }

    return true;
}

CAmount CTransaction::GetValueOut() const
{
    // vout
    CAmount nValueOut = CTransactionBase::GetValueOut();

    for (std::vector<JSDescription>::const_iterator it(vjoinsplit.begin()); it != vjoinsplit.end(); ++it)
    {
        // NB: vpub_old "takes" money from the value pool just as outputs do
        nValueOut += it->vpub_old;

        if (!MoneyRange(it->vpub_old) || !MoneyRange(nValueOut))
            throw std::runtime_error("CTransaction::GetValueOut(): value out of range");
    }

    nValueOut += (GetValueCcOut(vsc_ccout) + GetValueCcOut(vcl_ccout) + GetValueCcOut(vft_ccout));
    return nValueOut;
}

CAmount CTransaction::GetJoinSplitValueIn() const
{
    CAmount nValue = 0;
    for (std::vector<JSDescription>::const_iterator it(vjoinsplit.begin()); it != vjoinsplit.end(); ++it)
    {
        // NB: vpub_new "gives" money to the value pool just as inputs do
        nValue += it->vpub_new;

        if (!MoneyRange(it->vpub_new) || !MoneyRange(nValue))
            throw std::runtime_error("CTransaction::GetJoinSplitValueIn(): value out of range");
    }

    return nValue;
}

unsigned int CTransaction::CalculateSize() const
{
    unsigned int sz = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
//    LogPrint("cert", "%s():%d -sz=%u\n", __func__, __LINE__, sz);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << *this;
//    LogPrint("cert", "%s():%d -hex=%s\n", __func__, __LINE__, HexStr(ss.begin(), ss.end()) );
    return sz;
}

std::string CTransaction::ToString() const
{
    std::string str;

    if (IsScVersion())
    {
        str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%u, vout.size=%u, vsc_ccout.size=%u, vcl_ccout.size=%u, vft_ccout.size=%u, nLockTime=%u)\n",
            GetHash().ToString().substr(0,10),
            nVersion,
            vin.size(),
            vout.size(),
            vsc_ccout.size(),
            vcl_ccout.size(),
            vft_ccout.size(),
            nLockTime);

        for (unsigned int i = 0; i < vin.size(); i++)
            str += "    " + vin[i].ToString() + "\n";
        for (unsigned int i = 0; i < vout.size(); i++)
            str += "    " + vout[i].ToString() + "\n";
        for (unsigned int i = 0; i < vsc_ccout.size(); i++)
            str += "    " + vsc_ccout[i].ToString() + "\n";
        for (unsigned int i = 0; i < vcl_ccout.size(); i++)
            str += "    " + vcl_ccout[i].ToString() + "\n";
        for (unsigned int i = 0; i < vft_ccout.size(); i++)
            str += "    " + vft_ccout[i].ToString() + "\n";
    }
    else
    {
        str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%u, vout.size=%u, nLockTime=%u)\n",
            GetHash().ToString().substr(0,10),
            nVersion,
            vin.size(),
            vout.size(),
            nLockTime);
        for (unsigned int i = 0; i < vin.size(); i++)
            str += "    " + vin[i].ToString() + "\n";
        for (unsigned int i = 0; i < vout.size(); i++)
            str += "    " + vout[i].ToString() + "\n";
    }
    return str;
}

void CTransaction::addToScCommitment(std::map<uint256, std::vector<uint256> >& mLeaves, std::set<uint256>& sScIds) const
{
    if (!IsScVersion())
    {
        return;
    }

    unsigned int nIdx = 0;
    LogPrint("sc", "%s():%d -getting leaves for vsc out\n", __func__, __LINE__);
    fillCrosschainOutput(vsc_ccout, nIdx, mLeaves, sScIds);

    LogPrint("sc", "%s():%d -getting leaves for vcl out\n", __func__, __LINE__);
    fillCrosschainOutput(vcl_ccout, nIdx, mLeaves, sScIds);

    LogPrint("sc", "%s():%d -getting leaves for vft out\n", __func__, __LINE__);
    fillCrosschainOutput(vft_ccout, nIdx, mLeaves, sScIds);

    LogPrint("sc", "%s():%d - nIdx[%d]\n", __func__, __LINE__, nIdx);
}

//--------------------------------------------------------------------------------------------
// binaries other than zend that are produced in the build, do not call these members and therefore do not
// need linking all of the related symbols. We use this macro as it is already defined with a similar purpose
// in zen-tx binary build configuration
#ifdef BITCOIN_TX
bool CTransactionBase::CheckOutputsAreStandard(int nHeight, std::string& reason) const { return true; }

bool CTransaction::TryPushToMempool(bool fLimitFree, bool fRejectAbsurdFee) {return true;}
void CTransaction::AddToBlock(CBlock* pblock) const { return; }
void CTransaction::AddToBlockTemplate(CBlockTemplate* pblocktemplate, CAmount fee, unsigned int sigops) const {return; }
CAmount CTransaction::GetValueIn(const CCoinsViewCache& view) const { return 0; }
bool CTransaction::CheckInputsLimit(size_t limit, size_t& n) const { return true; }
bool CTransaction::ContextualCheck(CValidationState& state, int nHeight, int dosLevel) const { return true; }
bool CTransaction::IsStandard(std::string& reason, int nHeight) const { return true; }
bool CTransaction::CheckFinal(int flags) const { return true; }
bool CTransaction::IsApplicableToState(CValidationState& state, int nHeight) const { return true; }
void CTransaction::HandleJoinSplitCommittments(ZCIncrementalMerkleTree& tree) const { return; };
void CTransaction::AddJoinSplitToJSON(UniValue& entry) const { return; }
void CTransaction::AddSidechainOutsToJSON(UniValue& entry) const { return; }
bool CTransaction::AreInputsStandard(CCoinsViewCache& view) const { return true; }
bool CTransaction::ContextualCheckInputs(CValidationState &state, const CCoinsViewCache &view, bool fScriptChecks,
          const CChain& chain, unsigned int flags, bool cacheStore, const Consensus::Params& consensusParams,
          std::vector<CScriptCheck> *pvChecks) const { return true;}
double CTransaction::GetPriority(const CCoinsViewCache &view, int nHeight) const { return 0.0; }
std::string CTransaction::EncodeHex() const { return ""; }

#else
//----- 
bool CTransaction::TryPushToMempool(bool fLimitFree, bool fRejectAbsurdFee)
{
    CValidationState state;
    return ::AcceptToMemoryPool(mempool, state, *this, fLimitFree, nullptr, fRejectAbsurdFee);
};

bool CTransactionBase::CheckOutputsAreStandard(int nHeight, std::string& reason) const
{
    unsigned int nDataOut = 0;
    txnouttype whichType;

    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        CheckBlockResult checkBlockResult;
        if (!::IsStandard(txout.scriptPubKey, whichType, checkBlockResult))
        {
            reason = "scriptpubkey";
            return false;
        }

        if (checkBlockResult.referencedHeight > 0)
        {
            if ( (nHeight - checkBlockResult.referencedHeight) < ::getCheckBlockAtHeightMinAge())
            {
                LogPrintf("%s():%d - referenced block h[%d], chain.h[%d], minAge[%d]\n",
                    __func__, __LINE__, checkBlockResult.referencedHeight, nHeight, ::getCheckBlockAtHeightMinAge() );
                reason = "scriptpubkey checkblockatheight: referenced block too recent";
                return false;
            }
        }

        // provide temporary replay protection for two minerconf windows during chainsplit
        if ( (!txout.isFromBackwardTransfer) && (!IsCoinBase()) && (!ForkManager::getInstance().isTransactionTypeAllowedAtHeight(chainActive.Height(), whichType))) {
            reason = "op-checkblockatheight-needed";
            return false;
        }

        if (whichType == TX_NULL_DATA || whichType == TX_NULL_DATA_REPLAY)
            nDataOut++;
        else if ((whichType == TX_MULTISIG) && (!fIsBareMultisigStd)) {
            reason = "bare-multisig";
            return false;
        } else if (txout.IsDust(::minRelayTxFee)) {
            if (Params().NetworkIDString() == "regtest")
            {
                // do not reject this tx in regtest, there are py tests intentionally using zero values
                // and expecting this to be processable
                LogPrintf("%s():%d - txout is dust, ignoring it because we are in regtest\n",
                    __func__, __LINE__);
            }
            else
            {
                reason = "dust";
                return false;
            }
        }
    }

    // only one OP_RETURN txout is permitted
    if (nDataOut > 1) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool CTransactionBase::CheckOutputsCheckBlockAtHeightOpCode(CValidationState& state) const
{
    // Check for vout's without OP_CHECKBLOCKATHEIGHT opcode
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        // if the output comes from a backward transfer (when we are a certificate), skip this check
        // but go on if the certificate txout is an ordinary one
        if (txout.isFromBackwardTransfer)
            continue;

        txnouttype whichType;
        ::IsStandard(txout.scriptPubKey, whichType);

        // provide temporary replay protection for two minerconf windows during chainsplit
        if (!IsCoinBase() && !ForkManager::getInstance().isTransactionTypeAllowedAtHeight(chainActive.Height(),whichType))
        {
            return state.DoS(0, error("%s: %s: %s is not activated at this block height %d. Transaction rejected. Tx id: %s", __FILE__, __func__, ::GetTxnOutputType(whichType), chainActive.Height(), GetHash().ToString()),
                REJECT_CHECKBLOCKATHEIGHT_NOT_FOUND, "op-checkblockatheight-needed");
        }
    }

    return true;
}

void CTransaction::AddToBlock(CBlock* pblock) const 
{
    LogPrint("cert", "%s():%d - adding to block tx %s\n", __func__, __LINE__, GetHash().ToString());
    pblock->vtx.push_back(*this);
}

void CTransaction::AddToBlockTemplate(CBlockTemplate* pblocktemplate, CAmount fee, unsigned int sigops) const
{
    LogPrint("cert", "%s():%d - adding to block templ tx %s, fee=%s, sigops=%u\n", __func__, __LINE__,
        GetHash().ToString(), FormatMoney(fee), sigops);
    pblocktemplate->vTxFees.push_back(fee);
    pblocktemplate->vTxSigOps.push_back(sigops);
}

CAmount CTransaction::GetValueIn(const CCoinsViewCache& view) const
{
    if (IsCoinBase())
        return 0;

    CAmount nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxIn& ctxin = vin[i];
        nResult += view.GetOutputFor(ctxin).nValue;
    }

    nResult += GetJoinSplitValueIn();

    return nResult;
}

bool CTransaction::CheckInputsLimit(size_t limit, size_t& n) const
{
    if (limit > 0) {
        n = vin.size();
        if (n > limit) {
            return false;
        }
    }
    return true;
}

bool CTransaction::ContextualCheck(CValidationState& state, int nHeight, int dosLevel) const
{
    return ::ContextualCheckTransaction(*this, state, nHeight, dosLevel);
}

bool CTransaction::IsStandard(std::string& reason, int nHeight) const
{
    return ::IsStandardTx(*this, reason, nHeight);
}

bool CTransaction::CheckFinal(int flags) const
{
    return ::CheckFinalTx(*this, flags);
}

bool CTransaction::IsApplicableToState(CValidationState& state, int notUsed) const
{
    //ABENEGIA: Fill state properly
    CCoinsViewCache view(pcoinsTip);
    return view.HaveScRequirements(*this);
}
    
void CTransaction::HandleJoinSplitCommittments(ZCIncrementalMerkleTree& tree) const
{
    BOOST_FOREACH(const JSDescription &joinsplit, vjoinsplit) {
        BOOST_FOREACH(const uint256 &note_commitment, joinsplit.commitments) {
            // Insert the note commitments into our temporary tree.
            tree.append(note_commitment);
        }
    }
}

void CTransaction::AddJoinSplitToJSON(UniValue& entry) const
{
    entry.push_back(Pair("vjoinsplit", TxJoinSplitToJSON(*this)));
}

void CTransaction::AddSidechainOutsToJSON(UniValue& entry) const
{
    Sidechain::AddSidechainOutsToJSON(*this, entry);
}

bool CTransaction::AreInputsStandard(CCoinsViewCache& view) const 
{
    return ::AreInputsStandard(*this, view); 
}

bool CTransaction::ContextualCheckInputs(CValidationState &state, const CCoinsViewCache &view, bool fScriptChecks,
          const CChain& chain, unsigned int flags, bool cacheStore, const Consensus::Params& consensusParams,
          std::vector<CScriptCheck> *pvChecks) const
{
    return ::ContextualCheckInputs(*this, state, view, fScriptChecks, chain, flags, cacheStore, consensusParams, pvChecks);
}

double CTransaction::GetPriority(const CCoinsViewCache &view, int nHeight) const
{
    return view.GetPriority(*this, nHeight);
}

std::string CTransaction::EncodeHex() const
{
    return EncodeHexTx(*this);
}

#endif // BITCOIN_TX
