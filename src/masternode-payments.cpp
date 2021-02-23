// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The YieldStakingWallet developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"
#include "addrman.h"
#include "chainparams.h"
#include "fs.h"
#include "budget/budgetmanager.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "netmessagemaker.h"
#include "net_processing.h"
#include "spork.h"
#include "sync.h"
#include "util.h"
#include "utilmoneystr.h"


/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;

RecursiveMutex cs_vecPayments;
RecursiveMutex cs_mapMasternodeBlocks;
RecursiveMutex cs_mapMasternodePayeeVotes;

static const int MNPAYMENTS_DB_VERSION = 1;

//
// CMasternodePaymentDB
//

CMasternodePaymentDB::CMasternodePaymentDB()
{
    pathDB = GetDataDir() / "mnpayments.dat";
    strMagicMessage = "MasternodePayments";
}

bool CMasternodePaymentDB::Write(const CMasternodePayments& objToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << MNPAYMENTS_DB_VERSION;
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathDB, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint(BCLog::MASTERNODE,"Written info to mnpayments.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CMasternodePaymentDB::ReadResult CMasternodePaymentDB::Read(CMasternodePayments& objToLoad)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathDB, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = fs::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (const std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    int version;
    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header
        ssObj >> version;
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode payement cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CMasternodePayments object
        ssObj >> objToLoad;
    } catch (const std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint(BCLog::MASTERNODE,"Loaded info from mnpayments.dat (dbversion=%d) %dms\n", version, GetTimeMillis() - nStart);
    LogPrint(BCLog::MASTERNODE,"  %s\n", objToLoad.ToString());

    return Ok;
}

uint256 CMasternodePaymentWinner::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << std::vector<unsigned char>(payee.begin(), payee.end());
    ss << nBlockHeight;
    ss << vinMasternode.prevout;
    ss << payeeLevel;
    ss << payeeVin.prevout;
    return ss.GetHash();
}

std::string CMasternodePaymentWinner::GetStrMessage() const
{
    return vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + HexStr(payee);
}

bool CMasternodePaymentWinner::IsValid(CNode* pnode, std::string& strError)
{
    CMasternode* pmn = mnodeman.Find(vinMasternode.prevout);

    if (!pmn) {
        strError = strprintf("Unknown Masternode %s", vinMasternode.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE,"CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode);
        return false;
    }

    if (pmn->protocolVersion < ActiveProtocol()) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, ActiveProtocol());
        LogPrint(BCLog::MASTERNODE,"CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(vinMasternode, nBlockHeight - 100, ActiveProtocol());

    if (n == -1) {
        strError = strprintf("Unknown Masternode (rank==-1) %s", vinMasternode.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE, "CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        //It's common to have masternodes mistakenly think they are in the top 10
        // We don't want to print all of these messages, or punish them unless they're way off
        if (n > MNPAYMENTS_SIGNATURES_TOTAL * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, n);
            LogPrint(BCLog::MASTERNODE,"CMasternodePaymentWinner::IsValid - %s\n", strError);
            if (masternodeSync.IsSynced()) Misbehaving(pnode->GetId(), 20);
        }
        return false;
    }

    return true;
}

void CMasternodePaymentWinner::Relay()
{
    CInv inv(MSG_MASTERNODE_WINNER, GetHash());
    g_connman->RelayInv(inv);
}

void DumpMasternodePayments()
{
    int64_t nStart = GetTimeMillis();

    CMasternodePaymentDB paymentdb;
    LogPrint(BCLog::MASTERNODE,"Writing info to mnpayments.dat...\n");
    paymentdb.Write(masternodePayments);

    LogPrint(BCLog::MASTERNODE,"Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool IsBlockValueValid(int nHeight, CAmount& nExpectedValue, CAmount nMinted)
{
    if (!masternodeSync.IsSynced()) {
        //there is no budget data to use to check anything
        //super blocks will always be on these blocks, max 100 per budgeting
        if (nHeight % Params().GetConsensus().nBudgetCycleBlocks < 100) {
            if (Params().NetworkID() == CBaseChainParams::TESTNET) {
                return true;
            }
            nExpectedValue += g_budgetman.GetTotalBudget(nHeight);
        }
    } else {
        // we're synced and have data so check the budget schedule
        // if the superblock spork is enabled
        if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
            // add current payee amount to the expected block value
            CAmount expectedPayAmount;
            if (g_budgetman.GetExpectedPayeeAmount(nHeight, expectedPayAmount)) {
                nExpectedValue += expectedPayAmount;
            }
        }
    }

    return nMinted <= nExpectedValue;
}

bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight)
{
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;

    if (!masternodeSync.IsSynced()) { //there is no budget data to use to check anything -- find the longest chain
        LogPrint(BCLog::MASTERNODE, "Client not synced, skipping block payee checks\n");
        return true;
    }

    const bool isPoSActive = Params().GetConsensus().NetworkUpgradeActive(nBlockHeight, Consensus::UPGRADE_POS);
    const CTransaction& txNew = *(isPoSActive ? block.vtx[1] : block.vtx[0]);

    //check if it's a budget block
    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
        if (g_budgetman.IsBudgetPaymentBlock(nBlockHeight)) {
            transactionStatus = g_budgetman.IsTransactionValid(txNew, block.GetHash(), nBlockHeight);
            if (transactionStatus == TrxValidationStatus::Valid) {
                return true;
            }

            if (transactionStatus == TrxValidationStatus::InValid) {
                LogPrint(BCLog::MASTERNODE,"Invalid budget payment detected %s\n", txNew.ToString().c_str());
                if (sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT))
                    return false;

                LogPrint(BCLog::MASTERNODE,"Budget enforcement is disabled, accepting block\n");
            }
        }
    }

    // If we end here the transaction was either TrxValidationStatus::InValid and Budget enforcement is disabled, or
    // a double budget payment (status = TrxValidationStatus::DoublePayment) was detected, or no/not enough masternode
    // votes (status = TrxValidationStatus::VoteThreshold) for a finalized budget were found
    // In all cases a masternode will get the payment for this block

    //check for masternode payee
    if (masternodePayments.IsTransactionValid(txNew, nBlockHeight))
        return true;
    LogPrint(BCLog::MASTERNODE,"Invalid mn payment detected %s\n", txNew.ToString().c_str());

    if (sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT))
        return false;
    LogPrint(BCLog::MASTERNODE,"Masternode payment enforcement is disabled, accepting block\n");
    return true;
}


void FillBlockPayee(CMutableTransaction& txNew, const int nHeight, bool fProofOfStake)
{
    if (nHeight == 0) return;

    if (!sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) ||           // if superblocks are not enabled
            !g_budgetman.FillBlockPayee(txNew, nHeight, fProofOfStake) ) {    // or this is not a superblock,
        // ... or there's no budget with enough votes, then pay a masternode
        masternodePayments.FillBlockPayee(txNew, nHeight, fProofOfStake);
    }
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && g_budgetman.IsBudgetPaymentBlock(nBlockHeight)) {
        return g_budgetman.GetRequiredPaymentsString(nBlockHeight);
    } else {
        return masternodePayments.GetRequiredPaymentsString(nBlockHeight);
    }
}

void CMasternodePayments::FillBlockPayee(CMutableTransaction& txNew, const int nHeight, bool fProofOfStake)
{
    if (nHeight == 0) return;

    const bool payNewTiers = true; //IsSporkActive(SPORK_18_NEW_MASTERNODE_TIERS);
    unsigned int level = CMasternode::LevelValue::MIN; //1;
    unsigned int outputs = 1;
    CAmount mn_payments_total = 0;

    for (unsigned mnlevel = payNewTiers ? CMasternode::LevelValue::MIN : CMasternode::LevelValue::MAX; mnlevel <= CMasternode::LevelValue::MAX; ++mnlevel) {
        bool hasPayment = true;
        CScript payee;

        //spork
        if (!masternodePayments.GetBlockPayee(nHeight, mnlevel, payee)) {
            //no masternode detected
            const CMasternode* winningNode = mnodeman.GetCurrentMasterNode(mnlevel, 1);
            if (winningNode) {
                payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
            } else {
                LogPrint(BCLog::MASTERNODE,"CreateNewBlock: Failed to detect masternode level %d to pay\n", mnlevel);
                hasPayment = false;
            }
        }

        if (hasPayment) {
            const CAmount masternodePayment = GetMasternodePayment(mnlevel);
            if (fProofOfStake) {
                /**For Proof Of Stake vout[0] must be null
                 * Stake reward can be split into many different outputs, so we must
                 * use vout.size() to align with several different cases.
                 * An additional output is appended as the masternode payment
                 */
                unsigned int i = txNew.vout.size();
                if (level == 1)
                    outputs = i-1;
                txNew.vout.resize(i + 1);
                txNew.vout[i].scriptPubKey = payee;
                txNew.vout[i].nValue = masternodePayment;

                //subtract mn payment from the stake reward
                if (!txNew.vout[1].IsZerocoinMint()) {
                    if (outputs == 1) {
                        // Majority of cases; do it quick and move on
                        txNew.vout[1].nValue -= masternodePayment;
                    } else if (outputs > 1) {
                        // special case, stake is split between (i-1) outputs
                        CAmount mnPaymentSplit = masternodePayment / outputs;
                        CAmount mnPaymentRemainder = masternodePayment - (mnPaymentSplit * outputs);
                        for (unsigned int j=1; j<=outputs; j++) {
                            txNew.vout[j].nValue -= mnPaymentSplit;
                        }
                        // in case it's not an even division, take the last bit of dust from the last one
                        txNew.vout[outputs].nValue -= mnPaymentRemainder;
                    }
                }
            } else {
                txNew.vout.resize(1 + level);
                txNew.vout[level].scriptPubKey = payee;
                txNew.vout[level].nValue = masternodePayment;
                if (level == 1)
                    txNew.vout[0].nValue = GetBlockValue(nHeight) - masternodePayment;
                else
                    txNew.vout[0].nValue -= masternodePayment;
            }

            mn_payments_total += masternodePayment;
            CTxDestination address1;
            ExtractDestination(payee, address1);

            //if (payNewTiers)
                level++;

            LogPrint(BCLog::MASTERNODE,"Masternode payment of %s to %s\n", FormatMoney(masternodePayment).c_str(), EncodeDestination(address1).c_str());
        }
    }
}

void CMasternodePayments::ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (!masternodeSync.IsBlockchainSynced()) return;

    if (fLiteMode) return; //disable all Masternode related functionality


    if (strCommand == NetMsgType::GETMNWINNERS) { //Masternode Payments Request Sync
        if (fLiteMode) return;   //disable all Masternode related functionality

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (Params().NetworkID() == CBaseChainParams::MAIN) {
            if (pfrom->HasFulfilledRequest(NetMsgType::GETMNWINNERS)) {
                LogPrintf("CMasternodePayments::ProcessMessageMasternodePayments() : mnget - peer already asked me for the list\n");
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 20);
                return;
            }
        }

        pfrom->FulfilledRequest(NetMsgType::GETMNWINNERS);
        masternodePayments.Sync(pfrom, nCountNeeded);
        LogPrint(BCLog::MASTERNODE, "mnget - Sent Masternode winners to peer %i\n", pfrom->GetId());
    } else if (strCommand == NetMsgType::MNWINNER) { //Masternode Payments Declare Winner
        //this is required in litemodef
        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if (pfrom->nVersion < ActiveProtocol()) return;

        const int nHeight = mnodeman.GetBestHeight();

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);

        CMasternode* winner_mn = nullptr;

        // If the payeeVin is empty the winner object came from an old version, so we use the old logic
        if (winner.payeeVin == CTxIn()) {
            winner_mn = mnodeman.Find(winner.payee);

            if (winner_mn) {
                winner.payeeLevel = winner_mn->Level();
                winner.payeeVin = winner_mn->vin;
            }
        } else {
            winner_mn = mnodeman.Find(winner.payeeVin.prevout);
        }

        const std::string logString = strprintf("mnw - peer=%s ip=%s v=%u addr=%s winHeight=%u vin=%s",
            pfrom->GetId(),
            pfrom->addr.ToString().c_str(),
            pfrom->nVersion,
            EncodeDestination(address1).c_str(),
            winner.nBlockHeight,
            winner.vinMasternode.prevout.ToStringShort());

        if (!winner_mn) {
            LogPrint(BCLog::MASTERNODE, "%s - unknown payee - %s\n", logString.c_str(), EncodeDestination(address1).c_str());

            // Try to find the missing masternode
            // however DsegUpdate only asks once every 3h
            if (winner.payeeVin == CTxIn())
                mnodeman.DsegUpdate(pfrom);
            else
                mnodeman.AskForMN(pfrom, winner.payeeVin);

            return;
        }

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint(BCLog::MASTERNODE, "%s - Already seen - %s bestHeight %d\n", logString.c_str(), winner.GetHash().ToString().c_str(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        const int nFirstBlock = nHeight - (mnodeman.CountEnabled(winner.payeeLevel) * 1.25);
        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint(BCLog::MASTERNODE, "%s - winner out of range - FirstBlock %d Height %d bestHeight %d\n", logString.c_str(), nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        // reject old signature version
        if (winner.nMessVersion != MessageVersion::MESS_VER_HASH) {
            LogPrint(BCLog::MASTERNODE, "%s - rejecting old message version %d\n", logString.c_str(), winner.nMessVersion);
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pfrom, strError)) {
            if (strError != "") LogPrint(BCLog::MASTERNODE,"%s - invalid message - %s\n", logString.c_str(), strError);
            return;
        }

        if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight)) {
            LogPrint(BCLog::MASTERNODE,"%s - masternode already voted - %s\n", logString.c_str(), winner.vinMasternode.prevout.ToStringShort());
            return;
        }

        if (!winner.CheckSignature()) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("CMasternodePayments::ProcessMessageMasternodePayments() : %s - invalid signature\n", logString.c_str());
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, winner.vinMasternode);
            return;
        }

        LogPrint(BCLog::MASTERNODE, "%s - winning vote - Addr %s Height %d bestHeight %d - %s\n", logString.c_str(), EncodeDestination(address1).c_str(), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay();
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, unsigned mnlevel, CScript& payee)
{
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetPayee(mnlevel, payee);
    }

    return false;
}

// Is this masternode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 winners
bool CMasternodePayments::IsScheduled(const CMasternode& mn, int nNotBlockHeight) const
{
    LOCK(cs_mapMasternodeBlocks);

    const int nHeight = mnodeman.GetBestHeight();

    const CScript mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = nHeight; h <= nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapMasternodeBlocks.count(h)) {
            if (mapMasternodeBlocks.at(h).GetPayee(mn.Level(), payee)) {
                if (mnpayee == payee) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    // check winner height
    if (winnerIn.nBlockHeight - 100 > mnodeman.GetBestHeight() + 1) {
        return false;
    }

    {
        LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

        if (mapMasternodePayeeVotes.count(winnerIn.GetHash())) {
            return false;
        }

        mapMasternodePayeeVotes[winnerIn.GetHash()] = winnerIn;

        if (!mapMasternodeBlocks.count(winnerIn.nBlockHeight)) {
            CMasternodeBlockPayees blockPayees(winnerIn.nBlockHeight);
            mapMasternodeBlocks[winnerIn.nBlockHeight] = blockPayees;
        }
    }

    mapMasternodeBlocks[winnerIn.nBlockHeight].AddPayee(winnerIn.payeeLevel, winnerIn.payee, winnerIn.payeeVin, 1);

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CTransaction& txNew)
{
    LOCK(cs_vecPayments);

    // require at least 6 signatures
    std::map<unsigned, int> max_signatures;
    const bool payNewTiers = true; //IsSporkActive(SPORK_18_NEW_MASTERNODE_TIERS);
    for (CMasternodePayee& payee : vecPayments) {
        if (payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED || (!payNewTiers && payee.mnlevel != CMasternode::LevelValue::MAX))
            continue;

        std::pair<std::map<unsigned, int>::iterator, bool> ins_res = max_signatures.emplace(payee.mnlevel, payee.nVotes);

        if (ins_res.second)
            continue;

        if (payee.nVotes > ins_res.first->second)
            ins_res.first->second = payee.nVotes;
    }

    // if we don't have at least 6 signatures on a payee, approve whichever is the longest chain
    if (!max_signatures.size()) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePayments::IsTransactionValid - Not enough signatures, accepting\n");
        return true;
    }

    std::string strPayeesPossible = "";

    for (const CMasternodePayee& payee : vecPayments) {
        const CAmount requiredMasternodePayment = GetMasternodePayment(payee.mnlevel);

        if (strPayeesPossible != "")
            strPayeesPossible += ", ";

        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);

        strPayeesPossible += std::to_string(payee.mnlevel) + ":" + EncodeDestination(address1) + "(" + std::to_string(payee.nVotes) + ")=" + FormatMoney(requiredMasternodePayment).c_str();

        if (payee.nVotes < MNPAYMENTS_SIGNATURES_REQUIRED || (!payNewTiers && payee.mnlevel != CMasternode::LevelValue::MAX)) {
            LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid - Payment level %u found to %s vote=%u **\n", payee.mnlevel, EncodeDestination(address1), payee.nVotes);
            continue;
        }

        const auto& payee_out = std::find_if(txNew.vout.cbegin(), txNew.vout.cend(), [&payee, &requiredMasternodePayment](const CTxOut& out) {
            const bool is_payee = payee.scriptPubKey == out.scriptPubKey;
            const bool is_value_required = out.nValue == requiredMasternodePayment;

            if (is_payee && !is_value_required)
                LogPrint(BCLog::MASTERNODE,"%s : Masternode payment value (%s) different from required value (%s).\n",
                                __func__, FormatMoney(out.nValue).c_str(), FormatMoney(requiredMasternodePayment).c_str());

            return is_payee && is_value_required;
        });

        if (payee_out != txNew.vout.cend()) {
            const auto& it = max_signatures.find(payee.mnlevel);
            if (it != max_signatures.end())
                max_signatures.erase(payee.mnlevel);

            LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid - Payment level %u found to %s vote=%u\n", payee.mnlevel, EncodeDestination(address1), payee.nVotes);

            if (max_signatures.size())
                continue;

            LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid - Payment accepted to %s\n", strPayeesPossible.c_str());
            return true;
        }

        LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid - Payment level %u NOT found to %s vote=%u\n", payee.mnlevel, EncodeDestination(address1), payee.nVotes);
    }

    LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid - Missing required payment to %s\n", strPayeesPossible.c_str());
    LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid - TX Contents:\n");
    for (const CTxOut& out : txNew.vout) {
        CTxDestination address1;
        ExtractDestination(out.scriptPubKey, address1);
        LogPrint(BCLog::MASTERNODE,"CMasternodePayments::IsTransactionValid -     Address %s Value %s\n", EncodeDestination(address1), FormatMoney(out.nValue).c_str());
    }

    return false;
}

std::string CMasternodeBlockPayees::GetRequiredPaymentsString()
{
    LOCK(cs_vecPayments);

    std::string ret = "Unknown";

    for (CMasternodePayee& payee : vecPayments) {
        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);
        if (ret != "Unknown") {
            ret += ", ";
        }
        ret = EncodeDestination(address1) + ":" + std::to_string(payee.mnlevel) + ":" + std::to_string(payee.nVotes);
    }

    return ret;
}

std::string CMasternodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CMasternodePayments::IsTransactionValid(const CTransaction& txNew, int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CMasternodePayments::CleanPaymentList(int mnCount, int nHeight)
{
    LOCK2(cs_mapMasternodePayeeVotes, cs_mapMasternodeBlocks);

    //keep up to five cycles for historical sake
    int nLimit = std::max(int(mnCount * 1.25), 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint(BCLog::MASTERNODE, "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase((*it).first);
            mapMasternodePayeeVotes.erase(it++);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
        } else {
            ++it;
        }
    }
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if (!fMasterNode) return false;

    if (activeMasternode.vin == nullopt)
        return error("%s: Active Masternode not initialized.", __func__);

    //reference node - hybrid mode
    int n = mnodeman.GetMasternodeRank(*(activeMasternode.vin), nBlockHeight - 100, ActiveProtocol());

    if (n == -1) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint(BCLog::MASTERNODE, "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
        return false;
    }

    if (nBlockHeight <= nLastBlockHeight) return false;

    CPubKey pubKeyMasternode; CKey keyMasternode;
    activeMasternode.GetKeys(keyMasternode, pubKeyMasternode);

    std::vector<CMasternodePaymentWinner> winners;

    if (g_budgetman.IsBudgetPaymentBlock(nBlockHeight)) {
        //is budget payment block -- handled by the budgeting software
    } else {
        for (unsigned mnlevel = CMasternode::LevelValue::MIN; mnlevel <= CMasternode::LevelValue::MAX; ++mnlevel) {
            CMasternodePaymentWinner newWinner(*(activeMasternode.vin));

            LogPrint(BCLog::MASTERNODE,"CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s\n", nBlockHeight, activeMasternode.vin->prevout.hash.ToString());

            // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
            int nCount = 0;
            const CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment(nBlockHeight, mnlevel, true, nCount);

            if (pmn != nullptr) {
                LogPrint(BCLog::MASTERNODE,"CMasternodePayments::ProcessBlock() Found by FindOldestNotInVec\n");

                newWinner.nBlockHeight = nBlockHeight;

                CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());
                newWinner.AddPayee(payee, mnlevel, pmn->vin);

                CTxDestination address1;
                ExtractDestination(payee, address1);

                LogPrint(BCLog::MASTERNODE,"CMasternodePayments::ProcessBlock() Winner payee %s nHeight %i level %u\n", EncodeDestination(address1).c_str(), newWinner.nBlockHeight, mnlevel);

                LogPrint(BCLog::MASTERNODE,"CMasternodePayments::ProcessBlock() - Signing Winner level %u\n", mnlevel);
                if (!newWinner.Sign(keyMasternode, pubKeyMasternode))
                    continue;

                LogPrint(BCLog::MASTERNODE,"CMasternodePayments::ProcessBlock() - AddWinningMasternode level %u\n", mnlevel);
                if (!AddWinningMasternode(newWinner))
                    continue;

                winners.push_back(newWinner);
            } else {
                LogPrint(BCLog::MASTERNODE,"CMasternodePayments::ProcessBlock() Failed to find masternode level %u to pay\n", mnlevel);
                continue;
            }
        }
    }

    if (winners.empty())
        return false;

    for (CMasternodePaymentWinner& winner : winners) {
        winner.Relay();
    }

    nLastBlockHeight = nBlockHeight;

    return true;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded)
{
    LOCK(cs_mapMasternodePayeeVotes);

    const int nHeight = mnodeman.GetBestHeight();
    std::map<unsigned, int> mn_counts = mnodeman.CountEnabledByLevels();

    for (std::pair<const unsigned, int>& count : mn_counts)
        count.second = std::min(nCountNeeded, (int)(count.second * 1.25));

    int nInvCount = 0;
    for (const auto& vote : mapMasternodePayeeVotes) {
        const CMasternodePaymentWinner& winner = vote.second;

        const bool push = winner.nBlockHeight >= nHeight - mn_counts[winner.payeeLevel]
                  && winner.nBlockHeight <= nHeight + 20;

        if (!push)
            continue;

        node->PushInventory(CInv(MSG_MASTERNODE_WINNER, winner.GetHash()));
        ++nInvCount;
    }
    g_connman->PushMessage(node, CNetMsgMaker(node->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_MNW, nInvCount));
}

std::string CMasternodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapMasternodePayeeVotes.size() << ", Blocks: " << (int)mapMasternodeBlocks.size();

    return info.str();
}
