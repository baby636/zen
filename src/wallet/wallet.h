// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_WALLET_H
#define BITCOIN_WALLET_WALLET_H

#include "amount.h"
#include "coins.h"
#include "consensus/consensus.h"
#include "key.h"
#include "keystore.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/certificate.h"
#include "tinyformat.h"
#include "ui_interface.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "wallet/crypter.h"
#include "wallet/wallet_ismine.h"
#include "wallet/walletdb.h"
#include "zcash/Address.hpp"
#include "base58.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include "sc/sidechainrpc.h"

/**
 * Settings
 */
extern CFeeRate payTxFee;
extern CAmount maxTxFee;
extern unsigned int nTxConfirmTarget;
extern bool bSpendZeroConfChange;
extern bool fSendFreeTransactions;
extern bool fPayAtLeastCustomFee;

//! -paytxfee default
static const CAmount DEFAULT_TRANSACTION_FEE = 0;
//! -paytxfee will warn if called with a higher fee than this amount (in satoshis) per KB
static const CAmount nHighTransactionFeeWarning = 0.01 * COIN;
//! -maxtxfee default
static const CAmount DEFAULT_TRANSACTION_MAXFEE = 0.1 * COIN;
//! -txconfirmtarget default
static const unsigned int DEFAULT_TX_CONFIRM_TARGET = 2;
//! -maxtxfee will warn if called with a higher fee than this amount (in satoshis)
static const CAmount nHighTransactionMaxFeeWarning = 100 * nHighTransactionFeeWarning;
//! Largest (in bytes) free transaction we're willing to create
static const unsigned int MAX_FREE_TRANSACTION_CREATE_SIZE = 1000;
//! Size of witness cache
//  Should be large enough that we can expect not to reorg beyond our cache
//  unless there is some exceptional network disruption.
static const unsigned int WITNESS_CACHE_SIZE = COINBASE_MATURITY;

class CBlockIndex;
class CCoinControl;
class COutput;
class CReserveKey;
class CScript;
class CTxMemPool;
class CWalletTx;

/** (client) version numbers for particular wallet features */
enum WalletFeature
{
    FEATURE_BASE = 10500, // the earliest version new wallets supports (only useful for getinfo's clientversion output)

    FEATURE_WALLETCRYPT = 40000, // wallet encryption
    FEATURE_COMPRPUBKEY = 60000, // compressed public keys

    FEATURE_LATEST = 60000
};


/** A key pool entry */
class CKeyPool
{
public:
    int64_t nTime;
    CPubKey vchPubKey;

    CKeyPool();
    CKeyPool(const CPubKey& vchPubKeyIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(nTime);
        READWRITE(vchPubKey);
    }
};

/** Address book data */
class CAddressBookData
{
public:
    std::string name;
    std::string purpose;

    CAddressBookData()
    {
        purpose = "unknown";
    }

    typedef std::map<std::string, std::string> StringMap;
    StringMap destdata;
};

struct CRecipient
{
    CScript scriptPubKey;
    CAmount nAmount;
    bool fSubtractFeeFromAmount;
};

typedef std::map<std::string, std::string> mapValue_t;


static void ReadOrderPos(int64_t& nOrderPos, mapValue_t& mapValue)
{
    if (!mapValue.count("n"))
    {
        nOrderPos = -1; // TODO: calculate elsewhere
        return;
    }
    nOrderPos = atoi64(mapValue["n"].c_str());
}


static void WriteOrderPos(const int64_t& nOrderPos, mapValue_t& mapValue)
{
    if (nOrderPos == -1)
        return;
    mapValue["n"] = i64tostr(nOrderPos);
}

struct CScOutputEntry
{
    uint256 address;
    CAmount amount;
};

struct COutputEntry
{
    CTxDestination destination;
    CAmount amount;
    int vout;
};

/** An note outpoint */
class JSOutPoint
{
public:
    // Transaction hash
    uint256 hash;
    // Index into CTransaction.vjoinsplit
    uint64_t js;
    // Index into JSDescription fields of length ZC_NUM_JS_OUTPUTS
    uint8_t n;

    JSOutPoint() { SetNull(); }
    JSOutPoint(uint256 h, uint64_t js, uint8_t n) : hash {h}, js {js}, n {n} { }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(hash);
        READWRITE(js);
        READWRITE(n);
    }

    void SetNull() { hash.SetNull(); }
    bool IsNull() const { return hash.IsNull(); }

    friend bool operator<(const JSOutPoint& a, const JSOutPoint& b) {
        return (a.hash < b.hash ||
                (a.hash == b.hash && a.js < b.js) ||
                (a.hash == b.hash && a.js == b.js && a.n < b.n));
    }

    friend bool operator==(const JSOutPoint& a, const JSOutPoint& b) {
        return (a.hash == b.hash && a.js == b.js && a.n == b.n);
    }

    friend bool operator!=(const JSOutPoint& a, const JSOutPoint& b) {
        return !(a == b);
    }

    std::string ToString() const;
};

class CNoteData
{
public:
    libzcash::PaymentAddress address;

    /**
     * Cached note nullifier. May not be set if the wallet was not unlocked when
     * this was CNoteData was created. If not set, we always assume that the
     * note has not been spent.
     *
     * It's okay to cache the nullifier in the wallet, because we are storing
     * the spending key there too, which could be used to derive this.
     * If the wallet is encrypted, this means that someone with access to the
     * locked wallet cannot spend notes, but can connect received notes to the
     * transactions they are spent in. This is the same security semantics as
     * for transparent addresses.
     */
    boost::optional<uint256> nullifier;

    /**
     * Cached incremental witnesses for spendable Notes.
     * Beginning of the list is the most recent witness.
     */
    std::list<ZCIncrementalWitness> witnesses;

    /**
     * Block height corresponding to the most current witness.
     *
     * When we first create a CNoteData in CWallet::FindMyNotes, this is set to
     * -1 as a placeholder. The next time CWallet::ChainTip is called, we can
     * determine what height the witness cache for this note is valid for (even
     * if no witnesses were cached), and so can set the correct value in
     * CWallet::IncrementNoteWitnesses and CWallet::DecrementNoteWitnesses.
     */
    int witnessHeight;

    CNoteData() : address(), nullifier(), witnessHeight {-1} { }
    CNoteData(libzcash::PaymentAddress a) :
            address {a}, nullifier(), witnessHeight {-1} { }
    CNoteData(libzcash::PaymentAddress a, uint256 n) :
            address {a}, nullifier {n}, witnessHeight {-1} { }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(address);
        READWRITE(nullifier);
        READWRITE(witnesses);
        READWRITE(witnessHeight);
    }

    friend bool operator<(const CNoteData& a, const CNoteData& b) {
        return (a.address < b.address ||
                (a.address == b.address && a.nullifier < b.nullifier));
    }

    friend bool operator==(const CNoteData& a, const CNoteData& b) {
        return (a.address == b.address && a.nullifier == b.nullifier);
    }

    friend bool operator!=(const CNoteData& a, const CNoteData& b) {
        return !(a == b);
    }
};

typedef std::map<JSOutPoint, CNoteData> mapNoteData_t;

/** Decrypted note and its location in a transaction. */
struct CNotePlaintextEntry
{
    JSOutPoint jsop;
    libzcash::NotePlaintext plaintext;
};


class MerkleAbstractBase : virtual public CTransactionBase
{
protected:
    virtual int GetDepthInMainChainINTERNAL(const CBlockIndex* &pindexRet) const = 0;
public:
    uint256 hashBlock;
    std::vector<uint256> vMerkleBranch;
    int nIndex;

    // memory only
    mutable bool fMerkleVerified;

    void Init()
    {
        hashBlock = uint256();
        nIndex = -1;
        fMerkleVerified = false;
    }

    virtual uint256 GetObjHash() const = 0;
    virtual int SetMerkleBranch(const CBlock& block) = 0;
    virtual int GetBlocksToMaturity() const = 0;
    virtual bool AcceptToMemoryPool(bool fLimitFree=true, bool fRejectAbsurdFee=true);

    int GetDepthInMainChain(const CBlockIndex* &pindexRet) const;
    int GetDepthInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChain(pindexRet); }
    bool IsInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChainINTERNAL(pindexRet) > 0; }

    MerkleAbstractBase& operator=(const MerkleAbstractBase& m)
    {
        CTransactionBase::operator=(m);
        return *this;
    }
    MerkleAbstractBase& operator=(MerkleAbstractBase&& m)
    {
        CTransactionBase::operator=(m);
        return *this;
    }
    MerkleAbstractBase(const MerkleAbstractBase&) = default;
    MerkleAbstractBase() = default;
};

/** A transaction with a merkle branch linking it to the block chain. */
class CMerkleTx : public CTransaction, virtual public MerkleAbstractBase
{
private:
    int GetDepthInMainChainINTERNAL(const CBlockIndex* &pindexRet) const override;

#if 0
public:
    uint256 hashBlock;
    std::vector<uint256> vMerkleBranch;
    int nIndex;

    // memory only
    mutable bool fMerkleVerified;
#endif

public:
    CMerkleTx(const CMerkleTx&) = default;
    CMerkleTx& operator=(const CMerkleTx& tx)
    {
        CTransaction::operator=(tx);
        MerkleAbstractBase::operator=(tx);
        return *this;
    }

    CMerkleTx()
    {
        Init();
    }

    CMerkleTx(const CTransaction& txIn) : CTransaction(txIn)
    {
        Init();
    }

#if 0
    void Init()
    {
        hashBlock = uint256();
        nIndex = -1;
        fMerkleVerified = false;
    }
#endif

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(*(CTransaction*)this);
        nVersion = this->nVersion;
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
    }

    int SetMerkleBranch(const CBlock& block) override;

    uint256 GetObjHash() const override { return GetHash(); }

    /**
     * Return depth of transaction in blockchain:
     * -1  : not in blockchain, and not in memory pool (conflicted transaction)
     *  0  : in memory pool, waiting to be included in a block
     * >=1 : this many blocks deep in the main chain
     */
#if 0
    int GetDepthInMainChain(const CBlockIndex* &pindexRet) const;
    int GetDepthInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChain(pindexRet); }
    bool IsInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChainINTERNAL(pindexRet) > 0; }
#endif
    int GetBlocksToMaturity() const override;
//    bool AcceptToMemoryPool(bool fLimitFree=true, bool fRejectAbsurdFee=true);
};

/** A certificate with a merkle branch linking it to the block chain. */
class CMerkleCert : public CScCertificate, virtual public MerkleAbstractBase
{
private:
    int GetDepthInMainChainINTERNAL(const CBlockIndex* &pindexRet) const override;

public:
    CMerkleCert(const CMerkleCert&) = default;
    CMerkleCert& operator=(const CMerkleCert& cert)
    {
        CScCertificate::operator=(cert);
        MerkleAbstractBase::operator=(cert);
        return *this;
    }

    CMerkleCert()
    {
        Init();
    }

    CMerkleCert(const CScCertificate& certIn) : CScCertificate(certIn)
    {
        Init();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nLocVersion) {
        READWRITE(*(CScCertificate*)this);
        nLocVersion = this->nVersion;
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
    }

    int SetMerkleBranch(const CBlock& block) override;

    uint256 GetObjHash() const { return GetHash(); }

    /**
     * Return depth of transaction in blockchain:
     * -1  : not in blockchain, and not in memory pool (conflicted transaction)
     *  0  : in memory pool, waiting to be included in a block
     * >=1 : this many blocks deep in the main chain
     */
#if 0
    int GetDepthInMainChain(const CBlockIndex* &pindexRet) const;
    int GetDepthInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChain(pindexRet); }
    bool IsInMainChain() const { const CBlockIndex *pindexRet; return GetDepthInMainChainINTERNAL(pindexRet) > 0; }
#endif
    int GetBlocksToMaturity() const override;
//    bool AcceptToMemoryPool(bool fLimitFree=true, bool fRejectAbsurdFee=true);
};

class CWalletObjBase : virtual public MerkleAbstractBase
{
protected:
    const CWallet* pwallet;

public:
    // memory only
    mutable bool fDebitCached;
    mutable bool fCreditCached;
    mutable bool fImmatureCreditCached;
    mutable bool fAvailableCreditCached;
    mutable bool fWatchDebitCached;
    mutable bool fWatchCreditCached;
    mutable bool fImmatureWatchCreditCached;
    mutable bool fAvailableWatchCreditCached;
    mutable bool fChangeCached;
    mutable CAmount nDebitCached;
    mutable CAmount nCreditCached;
    mutable CAmount nImmatureCreditCached;
    mutable CAmount nAvailableCreditCached;
    mutable CAmount nWatchDebitCached;
    mutable CAmount nWatchCreditCached;
    mutable CAmount nImmatureWatchCreditCached;
    mutable CAmount nAvailableWatchCreditCached;
    mutable CAmount nChangeCached;
/*
    ~CWalletObjBase() {}
    CWalletObjBase() {}
    CWalletObjBase(const CWalletObjBase& wob) : MerkleAbstractBase(wob) {}
*/
#if 1
    CWalletObjBase& operator=(const CWalletObjBase& o)
    {
        MerkleAbstractBase::operator=(o);
        return *this;
    }

    CWalletObjBase& operator=(CWalletObjBase&& wob)
    {
        pwallet = wob.pwallet;

        fDebitCached                 = wob.fDebitCached;
        fCreditCached                = wob.fCreditCached;
        fImmatureCreditCached        = wob.fImmatureCreditCached;
        fAvailableCreditCached       = wob.fAvailableCreditCached;
        fWatchDebitCached            = wob.fWatchDebitCached;
        fWatchCreditCached           = wob.fWatchCreditCached;
        fImmatureWatchCreditCached   = wob.fImmatureWatchCreditCached;
        fAvailableWatchCreditCached  = wob.fAvailableWatchCreditCached;
        fChangeCached                = wob.fChangeCached;
        nDebitCached                 = wob.nDebitCached;
        nCreditCached                = wob.nCreditCached;
        nImmatureCreditCached        = wob.nImmatureCreditCached;
        nAvailableCreditCached       = wob.nAvailableCreditCached;
        nWatchDebitCached            = wob.nWatchDebitCached;
        nWatchCreditCached           = wob.nWatchCreditCached;
        nImmatureWatchCreditCached   = wob.nImmatureWatchCreditCached;
        nAvailableWatchCreditCached  = wob.nAvailableWatchCreditCached;
        nChangeCached                = wob.nChangeCached;

        return *this;
    }
#else
    CWalletObjBase& operator=(CWalletObjBase&&) = default;
#endif
    CWalletObjBase(const CWalletObjBase&) = default;
    CWalletObjBase() = default;

    mapValue_t mapValue;
    std::vector<std::pair<std::string, std::string> > vOrderForm; // ???
    unsigned int fTimeReceivedIsTxTime;
    unsigned int nTimeReceived; //! time received by this node
    unsigned int nTimeSmart;
    char fFromMe;
    std::string strFromAccount;
    int64_t nOrderPos; //! position in ordered transaction list

    void BindWallet(CWallet *pwalletIn)
    {
        pwallet = pwalletIn;
        MarkDirty();
    }

    bool IsFromMe(const isminefilter& filter) const
    {
        return (GetDebit(filter) > 0);
    }

    void GetAccountAmounts(const std::string& strAccount, CAmount& nReceived,
                           CAmount& nSent, CAmount& nFee, const isminefilter& filter) const;

    CAmount GetAvailableCredit(bool fUseCache=true) const;

    // virtuals
    virtual void MarkDirty()
    {
        fCreditCached = false;
        fAvailableCreditCached = false;
        fWatchDebitCached = false;
        fWatchCreditCached = false;
        fAvailableWatchCreditCached = false;
        fImmatureWatchCreditCached = false;
        fDebitCached = false;
        fChangeCached = false;
    }

    virtual void SetNoteData(mapNoteData_t &noteData) {}; // default is null

    //! filter decides which addresses will count towards the debit
    virtual CAmount GetDebit(const isminefilter& filter) const { return 0; } // certs has no vin
    virtual CAmount GetCredit(const isminefilter& filter) const; // base default is ok for certs
    virtual CAmount GetImmatureCredit(bool fUseCache=true) const = 0;
    virtual CAmount GetImmatureWatchOnlyCredit(const bool& fUseCache=true) const = 0;
    virtual CAmount GetAvailableWatchOnlyCredit(const bool& fUseCache=true) const = 0;
    virtual CAmount GetChange() const = 0;

    virtual void GetAmounts(std::list<COutputEntry>& listReceived, std::list<COutputEntry>& listSent, std::list<CScOutputEntry>& listScSent,
        CAmount& nFee, std::string& strSentAccount, const isminefilter& filter) const = 0;

    virtual bool IsTrusted() const = 0;

    virtual bool WriteToDisk(CWalletDB *pwalletdb) = 0;

    virtual int64_t GetTxTime() const = 0;
    virtual int GetRequestCount() const = 0;

    virtual bool RelayWalletTransaction() = 0;

    virtual std::set<uint256> GetConflicts() const { return std::set<uint256>(); } // default is the empty set (certs has no conflcts)
    virtual void GetConflicts(std::set<uint256>& result) const { } // default is empty (certs has no conflcts)
    virtual void GetNotesAmount(
        std::vector<CNotePlaintextEntry> & outEntries,
        bool fFilterAddress,
        libzcash::PaymentAddress filterPaymentAddress,
        bool ignoreSpent = true, bool ignoreUnspendable = true) {} // default empty (certs have no notes)
    virtual void AddToSpends(CWallet* pw) {}; // certs do not spend anything
    virtual void ClearNoteWitnessCache() {};
    // return false if the map is empty
    virtual const mapNoteData_t* GetMapNoteData() const { return NULL; }
    virtual void SetMapNoteData(mapNoteData_t& m) {}
    virtual void HandleInputGrouping(std::set< std::set<CTxDestination> >& groupings, std::set<CTxDestination>& grouping) {};

    void Init(const CWallet* pwalletIn)
    {
        pwallet = pwalletIn;
        mapValue.clear();
        //mapNoteData.clear();
        vOrderForm.clear();
        fTimeReceivedIsTxTime = false;
        nTimeReceived = 0;
        nTimeSmart = 0;
        fFromMe = false;
        strFromAccount.clear();
        fDebitCached = false;
        fCreditCached = false;
        fImmatureCreditCached = false;
        fAvailableCreditCached = false;
        fWatchDebitCached = false;
        fWatchCreditCached = false;
        fImmatureWatchCreditCached = false;
        fAvailableWatchCreditCached = false;
        fChangeCached = false;
        nDebitCached = 0;
        nCreditCached = 0;
        nImmatureCreditCached = 0;
        nAvailableCreditCached = 0;
        nWatchDebitCached = 0;
        nWatchCreditCached = 0;
        nAvailableWatchCreditCached = 0;
        nImmatureWatchCreditCached = 0;
        nChangeCached = 0;
        nOrderPos = -1;
    }

    virtual std::shared_ptr<CWalletObjBase> MakeWalletMapObject() const = 0;

};

/** 
 * A transaction with a bunch of additional info that only the owner cares about.
 * It includes any unrecorded transactions needed to link it back to the block chain.
 */
class CWalletTx : public CMerkleTx, virtual public CWalletObjBase
{
public:
    mapNoteData_t mapNoteData;

    CWalletTx& operator=(const CWalletTx& tx)
    {
        CMerkleTx::operator=(tx);
        CWalletObjBase::operator=(tx);

        mapNoteData = tx.mapNoteData;

        return *this;
    }

    CWalletTx()
    {
        Init(NULL);
        mapNoteData.clear();
    }

    CWalletTx(const CWallet* pwalletIn)
    {
        Init(pwalletIn);
        mapNoteData.clear();
    }

    CWalletTx(const CWallet* pwalletIn, const CMerkleTx& txIn) : CMerkleTx(txIn)
    {    
        Init(pwalletIn);
        mapNoteData.clear();
    }

    CWalletTx(const CWallet* pwalletIn, const CTransaction& txIn) : CMerkleTx(txIn)
    {
        Init(pwalletIn);
        mapNoteData.clear();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        if (ser_action.ForRead())
            Init(NULL);
        char fSpent = false;

        if (!ser_action.ForRead())
        {
            mapValue["fromaccount"] = strFromAccount;

            WriteOrderPos(nOrderPos, mapValue);

            if (nTimeSmart)
                mapValue["timesmart"] = strprintf("%u", nTimeSmart);
        }

        READWRITE(*(CMerkleTx*)this);
        std::vector<CMerkleTx> vUnused; //! Used to be vtxPrev
        READWRITE(vUnused);
        READWRITE(mapValue);
        READWRITE(mapNoteData);
        READWRITE(vOrderForm);
        READWRITE(fTimeReceivedIsTxTime);
        READWRITE(nTimeReceived);
        READWRITE(fFromMe);
        READWRITE(fSpent);

        if (ser_action.ForRead())
        {
            strFromAccount = mapValue["fromaccount"];

            ReadOrderPos(nOrderPos, mapValue);

            nTimeSmart = mapValue.count("timesmart") ? (unsigned int)atoi64(mapValue["timesmart"]) : 0;
        }

        mapValue.erase("fromaccount");
        mapValue.erase("version");
        mapValue.erase("spent");
        mapValue.erase("n");
        mapValue.erase("timesmart");
    }

    void SetNoteData(mapNoteData_t &noteData);

    //! filter decides which addresses will count towards the debit
    CAmount GetDebit(const isminefilter& filter) const override;
    CAmount GetCredit(const isminefilter& filter) const override;
    CAmount GetImmatureCredit(bool fUseCache=true) const override;
    CAmount GetImmatureWatchOnlyCredit(const bool& fUseCache=true) const override;
    CAmount GetAvailableWatchOnlyCredit(const bool& fUseCache=true) const override;
    CAmount GetChange() const override;

    void GetAmounts(std::list<COutputEntry>& listReceived, std::list<COutputEntry>& listSent, std::list<CScOutputEntry>& listScSent,
        CAmount& nFee, std::string& strSentAccount, const isminefilter& filter) const override;

    bool IsTrusted() const override;
    bool WriteToDisk(CWalletDB *pwalletdb) override;

    int64_t GetTxTime() const override;
    int GetRequestCount() const override;

    bool RelayWalletTransaction() override;

    std::set<uint256> GetConflicts() const override;
    void GetConflicts(std::set<uint256>& result) const override;

    void GetNotesAmount(
        std::vector<CNotePlaintextEntry> & outEntries,
        bool fFilterAddress,
        libzcash::PaymentAddress filterPaymentAddress,
        bool ignoreSpent = true, bool ignoreUnspendable = true); 
    void AddToSpends(CWallet* pw);
    void ClearNoteWitnessCache();
    const mapNoteData_t* GetMapNoteData() const;
    void SetMapNoteData(mapNoteData_t& m);
    void HandleInputGrouping(std::set< std::set<CTxDestination> >& groupings, std::set<CTxDestination>& grouping);

    // fill the crosschain output
    template <typename T>
    inline void fillScSent(const T& vOuts, std::list<CScOutputEntry>& listScSent) const
    {
        for(const auto& txccout : vOuts)
        {
            CScOutputEntry output = {txccout.address, txccout.nValue};
            listScSent.push_back(output);
        }
    }
    std::shared_ptr<CWalletObjBase> MakeWalletMapObject() const override;
};

class CWalletCert : public CMerkleCert, virtual public CWalletObjBase
{
public:

    CWalletCert& operator=(const CWalletCert& cert)
    {
        CMerkleCert::operator=(cert);
        CWalletObjBase::operator=(cert);

        // add members handling
        return *this;
    }

    CWalletCert()
    {
        Init(NULL);
    }

    CWalletCert(const CWallet* pwalletIn)
    {
        Init(pwalletIn);
    }

    CWalletCert(const CWallet* pwalletIn, const CMerkleCert& certIn) : CMerkleCert(certIn)
    {    
        Init(pwalletIn);
    }

    CWalletCert(const CWallet* pwalletIn, const CScCertificate& certIn) : CMerkleCert(certIn)
    {
        Init(pwalletIn);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        if (ser_action.ForRead())
            Init(NULL);
        char fSpent = false;

        if (!ser_action.ForRead())
        {
            mapValue["fromaccount"] = strFromAccount;

            WriteOrderPos(nOrderPos, mapValue);

            if (nTimeSmart)
                mapValue["timesmart"] = strprintf("%u", nTimeSmart);
        }

        READWRITE(*(CMerkleCert*)this);
        std::vector<CMerkleCert> vUnused; //! Used to be vtxPrev
        READWRITE(vUnused);
        READWRITE(mapValue);
        READWRITE(vOrderForm);
        READWRITE(fTimeReceivedIsTxTime);
        READWRITE(nTimeReceived);
        READWRITE(fFromMe);
        READWRITE(fSpent);

        if (ser_action.ForRead())
        {
            strFromAccount = mapValue["fromaccount"];

            ReadOrderPos(nOrderPos, mapValue);

            nTimeSmart = mapValue.count("timesmart") ? (unsigned int)atoi64(mapValue["timesmart"]) : 0;
        }

        mapValue.erase("fromaccount");
        mapValue.erase("version");
        mapValue.erase("spent");
        mapValue.erase("n");
        mapValue.erase("timesmart");
    }

    CAmount GetImmatureCredit(bool fUseCache=true) const override;
    CAmount GetImmatureWatchOnlyCredit(const bool& fUseCache=true) const override;
    CAmount GetAvailableWatchOnlyCredit(const bool& fUseCache=true) const override;
    CAmount GetChange() const override;

    void GetAmounts(std::list<COutputEntry>& listReceived, std::list<COutputEntry>& listSent, std::list<CScOutputEntry>& listScSent,
        CAmount& nFee, std::string& strSentAccount, const isminefilter& filter) const override;

    bool IsTrusted() const override;
    bool WriteToDisk(CWalletDB *pwalletdb) override;

    int64_t GetTxTime() const override;
    int GetRequestCount() const override;

    bool RelayWalletTransaction() override;

    std::shared_ptr<CWalletObjBase> MakeWalletMapObject() const override;
};




class COutput
{
public:
#if 0
    const CWalletTx *tx;
#else
    const CWalletObjBase *tx;
#endif
    int i;
    int nDepth;
    bool fSpendable;

#if 0
    COutput(const CWalletTx *txIn, int iIn, int nDepthIn, bool fSpendableIn)
#else
    COutput(const CWalletObjBase *txIn, int iIn, int nDepthIn, bool fSpendableIn)
#endif
    {
        tx = txIn; i = iIn; nDepth = nDepthIn; fSpendable = fSpendableIn;
    }

    std::string ToString() const;
};




/** Private key that includes an expiration date in case it never gets used. */
class CWalletKey
{
public:
    CPrivKey vchPrivKey;
    int64_t nTimeCreated;
    int64_t nTimeExpires;
    std::string strComment;
    //! todo: add something to note what created it (user, getnewaddress, change)
    //!   maybe should have a map<string, string> property map

    CWalletKey(int64_t nExpires=0);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(vchPrivKey);
        READWRITE(nTimeCreated);
        READWRITE(nTimeExpires);
        READWRITE(LIMITED_STRING(strComment, 65536));
    }
};

/**
 * Internal transfers.
 * Database key is acentry<account><counter>.
 */
class CAccountingEntry
{
public:
    std::string strAccount;
    CAmount nCreditDebit;
    int64_t nTime;
    std::string strOtherAccount;
    std::string strComment;
    mapValue_t mapValue;
    int64_t nOrderPos;  //! position in ordered transaction list
    uint64_t nEntryNo;

    CAccountingEntry()
    {
        SetNull();
    }

    void SetNull()
    {
        nCreditDebit = 0;
        nTime = 0;
        strAccount.clear();
        strOtherAccount.clear();
        strComment.clear();
        nOrderPos = -1;
        nEntryNo = 0;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        //! Note: strAccount is serialized as part of the key, not here.
        READWRITE(nCreditDebit);
        READWRITE(nTime);
        READWRITE(LIMITED_STRING(strOtherAccount, 65536));

        if (!ser_action.ForRead())
        {
            WriteOrderPos(nOrderPos, mapValue);

            if (!(mapValue.empty() && _ssExtra.empty()))
            {
                CDataStream ss(nType, nVersion);
                ss.insert(ss.begin(), '\0');
                ss << mapValue;
                ss.insert(ss.end(), _ssExtra.begin(), _ssExtra.end());
                strComment.append(ss.str());
            }
        }

        READWRITE(LIMITED_STRING(strComment, 65536));

        size_t nSepPos = strComment.find("\0", 0, 1);
        if (ser_action.ForRead())
        {
            mapValue.clear();
            if (std::string::npos != nSepPos)
            {
                CDataStream ss(std::vector<char>(strComment.begin() + nSepPos + 1, strComment.end()), nType, nVersion);
                ss >> mapValue;
                _ssExtra = std::vector<char>(ss.begin(), ss.end());
            }
            ReadOrderPos(nOrderPos, mapValue);
        }
        if (std::string::npos != nSepPos)
            strComment.erase(nSepPos);

        mapValue.erase("n");
    }

private:
    std::vector<char> _ssExtra;
};


/** 
 * A CWallet is an extension of a keystore, which also maintains a set of transactions and balances,
 * and provides the ability to create new transactions.
 */
class CWallet : public CCryptoKeyStore, public CValidationInterface
{
    friend class CWalletTx;
private:
#if 0
    bool SelectCoins(const CAmount& nTargetValue, std::set<std::pair<const CWalletTx*,unsigned int> >& setCoinsRet, CAmount& nValueRet, bool& fOnlyCoinbaseCoinsRet, bool& fNeedCoinbaseCoinsRet, const CCoinControl *coinControl = NULL) const;
#else
    bool SelectCoins(const CAmount& nTargetValue, std::set<std::pair<const CWalletObjBase*,unsigned int> >& setCoinsRet, CAmount& nValueRet, bool& fOnlyCoinbaseCoinsRet, bool& fNeedCoinbaseCoinsRet, const CCoinControl *coinControl = NULL) const;
#endif

    CWalletDB *pwalletdbEncryption;

    //! the current wallet version: clients below this version are not able to load the wallet
    int nWalletVersion;

    //! the maximum wallet format version: memory-only variable that specifies to what version this wallet may be upgraded
    int nWalletMaxVersion;

    int64_t nNextResend;
    int64_t nLastResend;
    bool fBroadcastTransactions;

    template <class T>
    using TxSpendMap = std::multimap<T, uint256>;
    /**
     * Used to keep track of spent outpoints, and
     * detect and report conflicts (double-spends or
     * mutated transactions where the mutant gets mined).
     */
    typedef TxSpendMap<COutPoint> TxSpends;
    TxSpends mapTxSpends;
    /**
     * Used to keep track of spent Notes, and
     * detect and report conflicts (double-spends).
     */
    typedef TxSpendMap<uint256> TxNullifiers;
    TxNullifiers mapTxNullifiers;

    void AddToSpends(const COutPoint& outpoint, const uint256& wtxid);
    void AddToSpends(const uint256& nullifier, const uint256& wtxid);
    void AddToSpends(const uint256& wtxid);

public:
    /*
     * Size of the incremental witness cache for the notes in our wallet.
     * This will always be greater than or equal to the size of the largest
     * incremental witness cache in any transaction in mapWallet.
     */
    int64_t nWitnessCacheSize;

    void ClearNoteWitnessCache();

protected:
    /**
     * pindex is the new tip being connected.
     */
    void IncrementNoteWitnesses(const CBlockIndex* pindex,
                                const CBlock* pblock,
                                ZCIncrementalMerkleTree& tree);
    /**
     * pindex is the old tip being disconnected.
     */
    void DecrementNoteWitnesses(const CBlockIndex* pindex);

    template <typename WalletDB>
    void SetBestChainINTERNAL(WalletDB& walletdb, const CBlockLocator& loc) {
        if (!walletdb.TxnBegin()) {
            // This needs to be done atomically, so don't do it at all
            LogPrintf("SetBestChain(): Couldn't start atomic write\n");
            return;
        }
        try {
#if 0
            for (std::pair<const uint256, CWalletTx>& wtxItem : mapWallet) {
                if (!walletdb.WriteTx(wtxItem.first, wtxItem.second)) {
#else
            for (auto& wtxItem : mapWallet) {
                if (!walletdb.WriteTx(wtxItem.first, *(wtxItem.second))) {
#endif
                    LogPrintf("SetBestChain(): Failed to write CWalletTx, aborting atomic write\n");
                    walletdb.TxnAbort();
                    return;
                }
            }
            if (!walletdb.WriteWitnessCacheSize(nWitnessCacheSize)) {
                LogPrintf("SetBestChain(): Failed to write nWitnessCacheSize, aborting atomic write\n");
                walletdb.TxnAbort();
                return;
            }
            if (!walletdb.WriteBestBlock(loc)) {
                LogPrintf("SetBestChain(): Failed to write best block, aborting atomic write\n");
                walletdb.TxnAbort();
                return;
            }
        } catch (const std::exception &exc) {
            // Unexpected failure
            LogPrintf("SetBestChain(): Unexpected error during atomic write:\n");
            LogPrintf("%s\n", exc.what());
            walletdb.TxnAbort();
            return;
        }
        if (!walletdb.TxnCommit()) {
            // Couldn't commit all to db, but in-memory state is fine
            LogPrintf("SetBestChain(): Couldn't commit atomic write\n");
            return;
        }
    }

private:
    template <class T>
    void SyncMetaData(std::pair<typename TxSpendMap<T>::iterator, typename TxSpendMap<T>::iterator>);

protected:
#if 0
    bool UpdatedNoteData(const CWalletTx& wtxIn, CWalletTx& wtx);
#else
    bool UpdatedNoteData(const CWalletObjBase& wtxIn, CWalletObjBase& wtx);
#endif
    void MarkAffectedTransactionsDirty(const CTransaction& tx);

public:
    /*
     * Main wallet lock.
     * This lock protects all the fields added by CWallet
     *   except for:
     *      fFileBacked (immutable after instantiation)
     *      strWalletFile (immutable after instantiation)
     */
    mutable CCriticalSection cs_wallet;

    bool fFileBacked;
    std::string strWalletFile;

    std::set<int64_t> setKeyPool;
    std::map<CKeyID, CKeyMetadata> mapKeyMetadata;
    std::map<libzcash::PaymentAddress, CKeyMetadata> mapZKeyMetadata;

    typedef std::map<unsigned int, CMasterKey> MasterKeyMap;
    MasterKeyMap mapMasterKeys;
    unsigned int nMasterKeyMaxID;

    CWallet()
    {
        SetNull();
    }

    CWallet(const std::string& strWalletFileIn)
    {
        SetNull();

        strWalletFile = strWalletFileIn;
        fFileBacked = true;
    }

    ~CWallet()
    {
        delete pwalletdbEncryption;
        pwalletdbEncryption = NULL;
    }

    void SetNull()
    {
        nWalletVersion = FEATURE_BASE;
        nWalletMaxVersion = FEATURE_BASE;
        fFileBacked = false;
        nMasterKeyMaxID = 0;
        pwalletdbEncryption = NULL;
        nOrderPosNext = 0;
        nNextResend = 0;
        nLastResend = 0;
        nTimeFirstKey = 0;
        fBroadcastTransactions = false;
        nWitnessCacheSize = 0;
    }

    /**
     * The reverse mapping of nullifiers to notes.
     *
     * The mapping cannot be updated while an encrypted wallet is locked,
     * because we need the SpendingKey to create the nullifier (#1502). This has
     * several implications for transactions added to the wallet while locked:
     *
     * - Parent transactions can't be marked dirty when a child transaction that
     *   spends their output notes is updated.
     *
     *   - We currently don't cache any note values, so this is not a problem,
     *     yet.
     *
     * - GetFilteredNotes can't filter out spent notes.
     *
     *   - Per the comment in CNoteData, we assume that if we don't have a
     *     cached nullifier, the note is not spent.
     *
     * Another more problematic implication is that the wallet can fail to
     * detect transactions on the blockchain that spend our notes. There are two
     * possible cases in which this could happen:
     *
     * - We receive a note when the wallet is locked, and then spend it using a
     *   different wallet client.
     *
     * - We spend from a PaymentAddress we control, then we export the
     *   SpendingKey and import it into a new wallet, and reindex/rescan to find
     *   the old transactions.
     *
     * The wallet will only miss "pure" spends - transactions that are only
     * linked to us by the fact that they contain notes we spent. If it also
     * sends notes to us, or interacts with our transparent addresses, we will
     * detect the transaction and add it to the wallet (again without caching
     * nullifiers for new notes). As by default JoinSplits send change back to
     * the origin PaymentAddress, the wallet should rarely miss transactions.
     *
     * To work around these issues, whenever the wallet is unlocked, we scan all
     * cached notes, and cache any missing nullifiers. Since the wallet must be
     * unlocked in order to spend notes, this means that GetFilteredNotes will
     * always behave correctly within that context (and any other uses will give
     * correct responses afterwards), for the transactions that the wallet was
     * able to detect. Any missing transactions can be rediscovered by:
     *
     * - Unlocking the wallet (to fill all nullifier caches).
     *
     * - Restarting the node with -reindex (which operates on a locked wallet
     *   but with the now-cached nullifiers).
     */
    std::map<uint256, JSOutPoint> mapNullifiersToNotes;

    std::map<uint256, std::shared_ptr<CWalletObjBase> > mapWallet;
    typedef std::map<uint256, std::shared_ptr<CWalletObjBase> >::const_iterator MAP_WALLET_CONST_IT;
//    std::map<uint256, CWalletCert> mapWalletCert;

    int64_t nOrderPosNext;
    std::map<uint256, int> mapRequestCount;

    std::map<CTxDestination, CAddressBookData> mapAddressBook;

    CPubKey vchDefaultKey;

    std::set<COutPoint> setLockedCoins;

    int64_t nTimeFirstKey;

#if 0
    const CWalletTx* GetWalletTx(const uint256& hash) const;
#else
    const CWalletObjBase* GetWalletTx(const uint256& hash) const;
#endif

    //! check whether we are allowed to upgrade (or already support) to the named feature
    bool CanSupportFeature(enum WalletFeature wf) { AssertLockHeld(cs_wallet); return nWalletMaxVersion >= wf; }

    void AvailableCoins(std::vector<COutput>& vCoins, bool fOnlyConfirmed=true, const CCoinControl *coinControl = NULL, bool fIncludeZeroValue=false, bool fIncludeCoinBase=true, bool fIncludeCommunityFund=true) const;
#if 0
    bool SelectCoinsMinConf(const CAmount& nTargetValue, int nConfMine, int nConfTheirs, std::vector<COutput> vCoins, std::set<std::pair<const CWalletTx*,unsigned int> >& setCoinsRet, CAmount& nValueRet) const;
#else
    bool SelectCoinsMinConf(const CAmount& nTargetValue, int nConfMine, int nConfTheirs, std::vector<COutput> vCoins,
        std::set<std::pair<const CWalletObjBase*,unsigned int> >& setCoinsRet, CAmount& nValueRet) const;
#endif

    bool IsSpent(const uint256& hash, unsigned int n) const;
    bool IsSpent(const uint256& nullifier) const;

    bool IsLockedCoin(uint256 hash, unsigned int n) const;
    void LockCoin(COutPoint& output);
    void UnlockCoin(COutPoint& output);
    void UnlockAllCoins();
    void ListLockedCoins(std::vector<COutPoint>& vOutpts);

    /**
     * keystore implementation
     * Generate a new key
     */
    CPubKey GenerateNewKey();
    //! Adds a key to the store, and saves it to disk.
    bool AddKeyPubKey(const CKey& key, const CPubKey &pubkey);
    //! Adds a key to the store, without saving it to disk (used by LoadWallet)
    bool LoadKey(const CKey& key, const CPubKey &pubkey) { return CCryptoKeyStore::AddKeyPubKey(key, pubkey); }
    //! Load metadata (used by LoadWallet)
    bool LoadKeyMetadata(const CPubKey &pubkey, const CKeyMetadata &metadata);

    bool LoadMinVersion(int nVersion) { AssertLockHeld(cs_wallet); nWalletVersion = nVersion; nWalletMaxVersion = std::max(nWalletMaxVersion, nVersion); return true; }

    //! Adds an encrypted key to the store, and saves it to disk.
    bool AddCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret);
    //! Adds an encrypted key to the store, without saving it to disk (used by LoadWallet)
    bool LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret);
    bool AddCScript(const CScript& redeemScript);
    bool LoadCScript(const CScript& redeemScript);

    //! Adds a destination data tuple to the store, and saves it to disk
    bool AddDestData(const CTxDestination &dest, const std::string &key, const std::string &value);
    //! Erases a destination data tuple in the store and on disk
    bool EraseDestData(const CTxDestination &dest, const std::string &key);
    //! Adds a destination data tuple to the store, without saving it to disk
    bool LoadDestData(const CTxDestination &dest, const std::string &key, const std::string &value);
    //! Look up a destination data tuple in the store, return true if found false otherwise
    bool GetDestData(const CTxDestination &dest, const std::string &key, std::string *value) const;

    //! Adds a watch-only address to the store, and saves it to disk.
    bool AddWatchOnly(const CScript &dest);
    bool RemoveWatchOnly(const CScript &dest);
    //! Adds a watch-only address to the store, without saving it to disk (used by LoadWallet)
    bool LoadWatchOnly(const CScript &dest);

    bool Unlock(const SecureString& strWalletPassphrase);
    bool ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase);
    bool EncryptWallet(const SecureString& strWalletPassphrase);

    void GetKeyBirthTimes(std::map<CKeyID, int64_t> &mapKeyBirth) const;

    /**
      * ZKeys
      */
    //! Generates a new zaddr
    CZCPaymentAddress GenerateNewZKey();
    //! Adds spending key to the store, and saves it to disk
    bool AddZKey(const libzcash::SpendingKey &key);
    //! Adds spending key to the store, without saving it to disk (used by LoadWallet)
    bool LoadZKey(const libzcash::SpendingKey &key);
    //! Load spending key metadata (used by LoadWallet)
    bool LoadZKeyMetadata(const libzcash::PaymentAddress &addr, const CKeyMetadata &meta);
    //! Adds an encrypted spending key to the store, without saving it to disk (used by LoadWallet)
    bool LoadCryptedZKey(const libzcash::PaymentAddress &addr, const libzcash::ReceivingKey &rk, const std::vector<unsigned char> &vchCryptedSecret);
    //! Adds an encrypted spending key to the store, and saves it to disk (virtual method, declared in crypter.h)
    bool AddCryptedSpendingKey(const libzcash::PaymentAddress &address, const libzcash::ReceivingKey &rk, const std::vector<unsigned char> &vchCryptedSecret);

    //! Adds a viewing key to the store, and saves it to disk.
    bool AddViewingKey(const libzcash::ViewingKey &vk);
    bool RemoveViewingKey(const libzcash::ViewingKey &vk);
    //! Adds a viewing key to the store, without saving it to disk (used by LoadWallet)
    bool LoadViewingKey(const libzcash::ViewingKey &dest);

    /** 
     * Increment the next transaction order id
     * @return next transaction order id
     */
    int64_t IncOrderPosNext(CWalletDB *pwalletdb = NULL);

#if 0
    typedef std::pair<CWalletTx*, CAccountingEntry*> TxPair;
#else
    typedef std::pair<CWalletObjBase*, CAccountingEntry*> TxPair;
#endif
    typedef std::multimap<int64_t, TxPair > TxItems;

    /**
     * Get the wallet's activity log
     * @return multimap of ordered transactions and accounting entries
     * @warning Returned pointers are *only* valid within the scope of passed acentries
     */
    TxItems OrderedTxItems(std::list<CAccountingEntry>& acentries, std::string strAccount = "");

    void MarkDirty();
    bool UpdateNullifierNoteMap();
#if 0
    void UpdateNullifierNoteMapWithTx(const CWalletTx& wtx);
    bool AddToWallet(const CWalletTx& wtxIn, bool fFromLoadWallet, CWalletDB* pwalletdb);
#else
    void UpdateNullifierNoteMapWithTx(const CWalletObjBase& wtx);
    bool AddToWallet(const CWalletObjBase& wtxIn, bool fFromLoadWallet, CWalletDB* pwalletdb);
#endif
    void SyncTransaction(const CTransaction& tx, const CBlock* pblock);
    void SyncCertificate(const CScCertificate& cert, const CBlock* pblock);
    bool AddToWalletIfInvolvingMe(const CTransaction& tx, const CBlock* pblock, bool fUpdate);
    bool AddToWalletIfInvolvingMe(const CScCertificate& cert, const CBlock* pblock, bool fUpdate);
    void EraseFromWallet(const uint256 &hash);
    void WitnessNoteCommitment(
         std::vector<uint256> commitments,
         std::vector<boost::optional<ZCIncrementalWitness>>& witnesses,
         uint256 &final_anchor);
    int ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate = false);
    void ReacceptWalletTransactions();
    void ResendWalletTransactions(int64_t nBestBlockTime);
    std::vector<uint256> ResendWalletTransactionsBefore(int64_t nTime);
    CAmount GetBalance() const;
    CAmount GetUnconfirmedBalance() const;
    CAmount GetImmatureBalance() const;
    CAmount GetWatchOnlyBalance() const;
    CAmount GetUnconfirmedWatchOnlyBalance() const;
    CAmount GetImmatureWatchOnlyBalance() const;
    bool FundTransaction(CMutableTransaction& tx, CAmount& nFeeRet, int& nChangePosRet, std::string& strFailReason);
    bool CreateTransaction(
        const std::vector<CRecipient>& vecSend, const std::vector< Sidechain::CcRecipientVariant >& vecCcSend,
        CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet, int& nChangePosRet,
        std::string& strFailReason, const CCoinControl *coinControl = NULL, bool sign = true);

    bool CommitTransaction(CWalletTx& wtxNew, CReserveKey& reservekey);

    bool CreateCertificate(
        const uint256& scId,
        const std::vector< Sidechain::CcRecipientVariant >& vecCcSend,
        CWalletTx& wtxNew, CReserveKey& reservekey, CAmount& nFeeRet,
        std::string& strFailReason, bool sign = true);

    static CFeeRate minTxFee;
    static CAmount GetMinimumFee(unsigned int nTxBytes, unsigned int nConfirmTarget, const CTxMemPool& pool);

    bool NewKeyPool();
    bool TopUpKeyPool(unsigned int kpSize = 0);
    void ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool);
    void KeepKey(int64_t nIndex);
    void ReturnKey(int64_t nIndex);
    bool GetKeyFromPool(CPubKey &key);
    int64_t GetOldestKeyPoolTime();
    void GetAllReserveKeys(std::set<CKeyID>& setAddress) const;

    std::set< std::set<CTxDestination> > GetAddressGroupings();
    std::map<CTxDestination, CAmount> GetAddressBalances();

    std::set<CTxDestination> GetAccountAddresses(const std::string& strAccount) const;

    boost::optional<uint256> GetNoteNullifier(
        const JSDescription& jsdesc,
        const libzcash::PaymentAddress& address,
        const ZCNoteDecryption& dec,
        const uint256& hSig,
        uint8_t n) const;
    mapNoteData_t FindMyNotes(const CTransaction& tx) const;
    bool IsFromMe(const uint256& nullifier) const;
    void GetNoteWitnesses(
         std::vector<JSOutPoint> notes,
         std::vector<boost::optional<ZCIncrementalWitness>>& witnesses,
         uint256 &final_anchor);

    isminetype IsMine(const CTxIn& txin) const;
    CAmount GetDebit(const CTxIn& txin, const isminefilter& filter) const;
    isminetype IsMine(const CTxOut& txout) const;
    CAmount GetCredit(const CTxOut& txout, const isminefilter& filter) const;
    bool IsChange(const CTxOut& txout) const;
    CAmount GetChange(const CTxOut& txout) const;
    bool IsMine(const CTransactionBase& tx) const;
    /** should probably be renamed to IsRelevantToMe */
    bool IsFromMe(const CTransaction& tx) const;
    CAmount GetDebit(const CTransaction& tx, const isminefilter& filter) const;
#if 0
    CAmount GetCredit(const CTransaction& tx, const isminefilter& filter) const;
#else
    CAmount GetCredit(const CTransactionBase& tx, const isminefilter& filter) const;
#endif
    CAmount GetChange(const CTransaction& tx) const;
    void ChainTip(const CBlockIndex *pindex, const CBlock *pblock, ZCIncrementalMerkleTree tree, bool added);
    /** Saves witness caches and best block locator to disk. */
    void SetBestChain(const CBlockLocator& loc);

    DBErrors LoadWallet(bool& fFirstRunRet);
    DBErrors ZapWalletTx(std::vector<CWalletTx>& vWtx);

    bool SetAddressBook(const CTxDestination& address, const std::string& strName, const std::string& purpose);

    bool DelAddressBook(const CTxDestination& address);

    void UpdatedTransaction(const uint256 &hashTx);

    void Inventory(const uint256 &hash)
    {
        {
            LOCK(cs_wallet);
            std::map<uint256, int>::iterator mi = mapRequestCount.find(hash);
            if (mi != mapRequestCount.end())
                (*mi).second++;
        }
    }

    unsigned int GetKeyPoolSize()
    {
        AssertLockHeld(cs_wallet); // setKeyPool
        return setKeyPool.size();
    }

    bool SetDefaultKey(const CPubKey &vchPubKey);

    //! signify that a particular wallet feature is now used. this may change nWalletVersion and nWalletMaxVersion if those are lower
    bool SetMinVersion(enum WalletFeature, CWalletDB* pwalletdbIn = NULL, bool fExplicit = false);

    //! change which version we're allowed to upgrade to (note that this does not immediately imply upgrading to that format)
    bool SetMaxVersion(int nVersion);

    //! get the current wallet format (the oldest client version guaranteed to understand this wallet)
    int GetVersion() { LOCK(cs_wallet); return nWalletVersion; }

    //! Get wallet transactions that conflict with given transaction (spend same outputs)
    std::set<uint256> GetConflicts(const uint256& txid) const;

    //! Flush wallet (bitdb flush)
    void Flush(bool shutdown=false);

    //! Verify the wallet database and perform salvage if required
    static bool Verify(const std::string& walletFile, std::string& warningString, std::string& errorString);
    
    /** 
     * Address book entry changed.
     * @note called with lock cs_wallet held.
     */
    boost::signals2::signal<void (CWallet *wallet, const CTxDestination
            &address, const std::string &label, bool isMine,
            const std::string &purpose,
            ChangeType status)> NotifyAddressBookChanged;

    /** 
     * Wallet transaction added, removed or updated.
     * @note called with lock cs_wallet held.
     */
    boost::signals2::signal<void (CWallet *wallet, const uint256 &hashTx,
            ChangeType status)> NotifyTransactionChanged;

    /** Show progress e.g. for rescan */
    boost::signals2::signal<void (const std::string &title, int nProgress)> ShowProgress;

    /** Watch-only address added */
    boost::signals2::signal<void (bool fHaveWatchOnly)> NotifyWatchonlyChanged;

    /** Inquire whether this wallet broadcasts transactions. */
    bool GetBroadcastTransactions() const { return fBroadcastTransactions; }
    /** Set whether this wallet broadcasts transactions. */
    void SetBroadcastTransactions(bool broadcast) { fBroadcastTransactions = broadcast; }
    
    /* Find notes filtered by payment address, min depth, ability to spend */
    void GetFilteredNotes(std::vector<CNotePlaintextEntry> & outEntries,
                          std::string address,
                          int minDepth=1,
                          bool ignoreSpent=true,
                          bool ignoreUnspendable=true);
    
};

/** A key allocated from the key pool. */
class CReserveKey
{
protected:
    CWallet* pwallet;
    int64_t nIndex;
    CPubKey vchPubKey;
public:
    CReserveKey(CWallet* pwalletIn)
    {
        nIndex = -1;
        pwallet = pwalletIn;
    }

    ~CReserveKey()
    {
        ReturnKey();
    }

    void ReturnKey();
    virtual bool GetReservedKey(CPubKey &pubkey);
    void KeepKey();
};


/** 
 * Account information.
 * Stored in wallet with key "acc"+string account name.
 */
class CAccount
{
public:
    CPubKey vchPubKey;

    CAccount()
    {
        SetNull();
    }

    void SetNull()
    {
        vchPubKey = CPubKey();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(vchPubKey);
    }
};

#endif // BITCOIN_WALLET_WALLET_H
