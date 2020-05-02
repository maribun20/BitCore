// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018-2018 The PiVX developers
// Copyright (c) 2019 Limxtec developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <validation.h>
#include <messagesigner.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <spork.h>
// FXTC BEGIN
// Pivx
#include <sporkdb.h>
//
// FXTC END

#include <boost/lexical_cast.hpp>

class CSporkMessage;
class CSporkManager;

CSporkManager sporkManager;

std::map<uint256, CSporkMessage> mapSporks;

// FXTC BEGIN
std::unique_ptr<CSporkDB> pSporkDB = NULL;

// PIVX: on startup load spork values from previous session if they exist in the sporkDB
void CSporkManager::LoadSporksFromDB()
{
    // FXTC BEGIN
    //for (int i = SPORK_START; i <= SPORK_END; ++i) {
    for (int i = SPORK_START; i <= SPORK_BTX_END; ++i) {
        if (i > SPORK_END && i < SPORK_BTX_START) i = SPORK_BTX_START;
    // FXTC END
        // Since not all spork IDs are in use, we have to exclude undefined IDs
        std::string strSpork = sporkManager.GetSporkNameByID(i);
        if (strSpork.substr(0,7) == "Unknown") continue;

        // attempt to read spork from sporkDB
        CSporkMessage spork;
        if (!pSporkDB->ReadSpork(i, spork)) {
            LogPrintf("%s : no previous value for %s found in database\n", __func__, strSpork);
            continue;
        }

        // add spork to memory
        mapSporks[spork.GetHash()] = spork;
        mapSporksActive[spork.nSporkID] = spork;
        std::time_t result = spork.nValue;
        // If SPORK Value is greater than 1,000,000 assume it's actually a Date and then convert to a more readable format
        if (spork.nValue > 1000000) {
            LogPrintf("%s : loaded spork %s with value %d : %s\n", __func__,
                      sporkManager.GetSporkNameByID(spork.nSporkID), spork.nValue,
                      std::ctime(&result));
        } else {
            LogPrintf("%s : loaded spork %s with value %d\n", __func__,
                      sporkManager.GetSporkNameByID(spork.nSporkID), spork.nValue);
        }
    }
}
// FXTC END

void CSporkManager::ProcessSpork(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if(fLiteMode) return; // disable all Dash specific functionality

    if (strCommand == NetMsgType::SPORK) {

        CSporkMessage spork;
        vRecv >> spork;

        uint256 hash = spork.GetHash();

        std::string strLogMsg;
        {
            LOCK(cs_main);
            pfrom->setAskFor.erase(hash);
            if(!chainActive.Tip()) return;
            strLogMsg = strprintf("SPORK -- hash: %s id: %d value: %10d bestHeight: %d peer=%d", hash.ToString(), spork.nSporkID, spork.nValue, chainActive.Height(), pfrom->GetId());
        }

        if(mapSporksActive.count(spork.nSporkID)) {
            if (mapSporksActive[spork.nSporkID].nTimeSigned >= spork.nTimeSigned) {
                LogPrint(BCLog::SPORK, "%s seen\n", strLogMsg);
                return;
            } else {
                LogPrintf("%s updated\n", strLogMsg);
            }
        } else {
            LogPrintf("%s new\n", strLogMsg);
        }

        if(!spork.CheckSignature()) {
            LOCK(cs_main);
            LogPrintf("CSporkManager::ProcessSpork -- invalid signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        mapSporks[hash] = spork;
        mapSporksActive[spork.nSporkID] = spork;
        spork.Relay(connman);

        //does a task if needed
        ExecuteSpork(spork.nSporkID, spork.nValue);

        // FXTC BEGIN
        // PIVX: add to spork database.
        pSporkDB->WriteSpork(spork.nSporkID, spork);
        // FXTC END
    } else if (strCommand == NetMsgType::GETSPORKS) {

        std::map<int, CSporkMessage>::iterator it = mapSporksActive.begin();

        while(it != mapSporksActive.end()) {
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SPORK, it->second));
            it++;
        }
    }

}

void CSporkManager::ExecuteSpork(int nSporkID, int nValue)
{
    //correct fork via spork technology
    if(nSporkID == SPORK_12_RECONSIDER_BLOCKS && nValue > 0) {
        // allow to reprocess 24h of blocks max, which should be enough to resolve any issues
        int64_t nMaxBlocks = 576;
        // this potentially can be a heavy operation, so only allow this to be executed once per 10 minutes
        int64_t nTimeout = 10 * 60;

        static int64_t nTimeExecuted = 0; // i.e. it was never executed before

        if(GetTime() - nTimeExecuted < nTimeout) {
            LogPrint(BCLog::SPORK, "CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider blocks, too soon - %d/%d\n", GetTime() - nTimeExecuted, nTimeout);
            return;
        }

        if(nValue > nMaxBlocks) {
            LogPrintf("CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider too many blocks %d/%d\n", nValue, nMaxBlocks);
            return;
        }


        LogPrintf("CSporkManager::ExecuteSpork -- Reconsider Last %d Blocks\n", nValue);

        ReprocessBlocks(nValue);
        nTimeExecuted = GetTime();
    }
}

bool CSporkManager::UpdateSpork(int nSporkID, int64_t nValue, CConnman& connman)
{

    CSporkMessage spork = CSporkMessage(nSporkID, nValue, GetAdjustedTime());

    if(spork.Sign(strMasterPrivKey)) {
        spork.Relay(connman);
        mapSporks[spork.GetHash()] = spork;
        mapSporksActive[nSporkID] = spork;
        return true;
    }

    return false;
}

// grab the spork, otherwise say it's off
bool CSporkManager::IsSporkActive(int nSporkID)
{
    int64_t r = -1;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].nValue;
    } else {
        switch (nSporkID) {
            case SPORK_2_INSTANTSEND_ENABLED:                      r = SPORK_2_INSTANTSEND_ENABLED_DEFAULT; break;
            case SPORK_3_INSTANTSEND_BLOCK_FILTERING:              r = SPORK_3_INSTANTSEND_BLOCK_FILTERING_DEFAULT; break;
            case SPORK_5_INSTANTSEND_MAX_VALUE:                    r = SPORK_5_INSTANTSEND_MAX_VALUE_DEFAULT; break;
            case SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:           r = SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT_DEFAULT; break;
            case SPORK_9_SUPERBLOCKS_ENABLED:                      r = SPORK_9_SUPERBLOCKS_ENABLED_DEFAULT; break;
            case SPORK_10_UNKNOW:            r = SPORK_10_UNKNOW_DEFAULT; break;
            case SPORK_12_RECONSIDER_BLOCKS:                       r = SPORK_12_RECONSIDER_BLOCKS_DEFAULT; break;
            case SPORK_13_OLD_SUPERBLOCK_FLAG:                     r = SPORK_13_OLD_SUPERBLOCK_FLAG_DEFAULT; break;
            case SPORK_14_UNKNOW:                                  r = SPORK_14_UNKNOW_DEFAULT; break;
            // FXTC BEGIN
            case SPORK_BTX_01_HANDBRAKE_HEIGHT :                   r = SPORK_BTX_01_HANDBRAKE_HEIGHT_DEFAULT; break;
            case SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D:             r = SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D_DEFAULT; break;
            case SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT:              r = SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT_DEFAULT; break;
            case SPORK_BTX_01_HANDBRAKE_FORCE_NIST5:               r = SPORK_BTX_01_HANDBRAKE_FORCE_NIST5_DEFAULT; break;
            case SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z:              r = SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z_DEFAULT; break;
            case SPORK_BTX_01_HANDBRAKE_FORCE_X11:                 r = SPORK_BTX_01_HANDBRAKE_FORCE_X11_DEFAULT; break;
            case SPORK_BTX_01_HANDBRAKE_FORCE_X16R:                r = SPORK_BTX_01_HANDBRAKE_FORCE_X16R_DEFAULT; break;

            case SPORK_BTX_15_COINBASE_MATURITY_STAGE_2:           r = SPORK_BTX_15_COINBASE_MATURITY_STAGE_2_DEFAULT; break;
            case SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK:           r = SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK_DEFAULT; break;
            case SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE:           r = SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE_DEFAULT; break;
            case SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT:          r = SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT_DEFAULT; break;
            case SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING:       r = SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING_DEFAULT; break;

            case SPORK_BTX_18_MIN_PEER_PROTO_VERSION:              r = SPORK_BTX_18_MIN_PEER_PROTO_VERSION_DEFAULT; break;
            case SPORK_BTX_19_MIN_VERSION_COUNT:                   r = SPORK_BTX_19_MIN_VERSION_COUNT_DEFAULT; break;
            case SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT:          r = SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT_DEFAULT; break;
            case SPORK_BTX_16_COINBASE_MATURITY_STAGE_3:                   r = SPORK_BTX_16_COINBASE_MATURITY_STAGE_3_DEFAULT; break;
            case SPORK_22_FACTOR_ENFORCEMENT:                      r = SPORK_22_FACTOR_ENFORCEMENT_DEFAULT; break;
            // FXTC END
            default:
                LogPrint(BCLog::SPORK, "CSporkManager::IsSporkActive -- Unknown Spork ID %d\n", nSporkID);
                r = 4070908800ULL; // 2099-1-1 i.e. off by default
                break;
        }
    }

    return r < GetAdjustedTime();
}

// grab the value of the spork on the network, or the default
int64_t CSporkManager::GetSporkValue(int nSporkID)
{
    if (mapSporksActive.count(nSporkID))
        return mapSporksActive[nSporkID].nValue;

    switch (nSporkID) {
        case SPORK_2_INSTANTSEND_ENABLED:                      return SPORK_2_INSTANTSEND_ENABLED_DEFAULT;
        case SPORK_3_INSTANTSEND_BLOCK_FILTERING:              return SPORK_3_INSTANTSEND_BLOCK_FILTERING_DEFAULT;
        case SPORK_5_INSTANTSEND_MAX_VALUE:                    return SPORK_5_INSTANTSEND_MAX_VALUE_DEFAULT;
        case SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:           return SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT_DEFAULT;
        case SPORK_9_SUPERBLOCKS_ENABLED:                      return SPORK_9_SUPERBLOCKS_ENABLED_DEFAULT;
        case SPORK_10_UNKNOW:            return SPORK_10_UNKNOW_DEFAULT;
        case SPORK_12_RECONSIDER_BLOCKS:                       return SPORK_12_RECONSIDER_BLOCKS_DEFAULT;
        case SPORK_13_OLD_SUPERBLOCK_FLAG:                     return SPORK_13_OLD_SUPERBLOCK_FLAG_DEFAULT;
        case SPORK_14_UNKNOW:                                  return SPORK_14_UNKNOW_DEFAULT;
        // FXTC BEGIN
        case SPORK_BTX_01_HANDBRAKE_HEIGHT:                    return SPORK_BTX_01_HANDBRAKE_HEIGHT_DEFAULT;
        case SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D:             return SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D_DEFAULT;
        case SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT:              return SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT_DEFAULT;
        case SPORK_BTX_01_HANDBRAKE_FORCE_NIST5:               return SPORK_BTX_01_HANDBRAKE_FORCE_NIST5_DEFAULT;
        case SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z:              return SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z_DEFAULT;
        case SPORK_BTX_01_HANDBRAKE_FORCE_X11:                 return SPORK_BTX_01_HANDBRAKE_FORCE_X11_DEFAULT;
        case SPORK_BTX_01_HANDBRAKE_FORCE_X16R:                return SPORK_BTX_01_HANDBRAKE_FORCE_X16R_DEFAULT;

        case SPORK_BTX_15_COINBASE_MATURITY_STAGE_2:           return SPORK_BTX_15_COINBASE_MATURITY_STAGE_2_DEFAULT;
        case SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK:           return SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK_DEFAULT;
        case SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE:           return SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE_DEFAULT;
        case SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT:          return SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT_DEFAULT;
        case SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING:       return SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING_DEFAULT;

        case SPORK_BTX_18_MIN_PEER_PROTO_VERSION:              return SPORK_BTX_18_MIN_PEER_PROTO_VERSION_DEFAULT;
        case SPORK_BTX_19_MIN_VERSION_COUNT:                   return SPORK_BTX_19_MIN_VERSION_COUNT_DEFAULT;
        case SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT:          return SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT_DEFAULT;
        case SPORK_BTX_16_COINBASE_MATURITY_STAGE_3:                   return SPORK_BTX_16_COINBASE_MATURITY_STAGE_3_DEFAULT;
        case SPORK_22_FACTOR_ENFORCEMENT:                      return SPORK_22_FACTOR_ENFORCEMENT_DEFAULT;

        default:
            LogPrint(BCLog::SPORK, "CSporkManager::GetSporkValue -- Unknown Spork ID %d\n", nSporkID);
            return -1;
    }

}

int CSporkManager::GetSporkIDByName(std::string strName)
{
    if (strName == "SPORK_2_INSTANTSEND_ENABLED")                return SPORK_2_INSTANTSEND_ENABLED;
    if (strName == "SPORK_3_INSTANTSEND_BLOCK_FILTERING")        return SPORK_3_INSTANTSEND_BLOCK_FILTERING;
    if (strName == "SPORK_5_INSTANTSEND_MAX_VALUE")              return SPORK_5_INSTANTSEND_MAX_VALUE;
    if (strName == "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")     return SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT;
    if (strName == "SPORK_9_SUPERBLOCKS_ENABLED")                return SPORK_9_SUPERBLOCKS_ENABLED;
    if (strName == "SPORK_10_UNKNOW")      return SPORK_10_UNKNOW;
    if (strName == "SPORK_12_RECONSIDER_BLOCKS")                 return SPORK_12_RECONSIDER_BLOCKS;
    if (strName == "SPORK_13_OLD_SUPERBLOCK_FLAG")               return SPORK_13_OLD_SUPERBLOCK_FLAG;
    if (strName == "SPORK_14_UNKNOW")                            return SPORK_14_UNKNOW;

    if (strName == "SPORK_BTX_01_HANDBRAKE_HEIGHT")              return SPORK_BTX_01_HANDBRAKE_HEIGHT;
    if (strName == "SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D")       return SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D;
    if (strName == "SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT")        return SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT;
    if (strName == "SPORK_BTX_01_HANDBRAKE_FORCE_NIST5")         return SPORK_BTX_01_HANDBRAKE_FORCE_NIST5;
    if (strName == "SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z")        return SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z;
    if (strName == "SPORK_BTX_01_HANDBRAKE_FORCE_X11")           return SPORK_BTX_01_HANDBRAKE_FORCE_X11;
    if (strName == "SPORK_BTX_01_HANDBRAKE_FORCE_X16R")          return SPORK_BTX_01_HANDBRAKE_FORCE_X16R;

    if (strName == "SPORK_BTX_15_COINBASE_MATURITY_STAGE_2")     return SPORK_BTX_15_COINBASE_MATURITY_STAGE_2;
    if (strName == "SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK")     return SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK;
    if (strName == "SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE")     return SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE;
    if (strName == "SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT")    return SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT;
    if (strName == "SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING") return SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING;

    if (strName == "SPORK_BTX_18_MIN_PEER_PROTO_VERSION")       return SPORK_BTX_18_MIN_PEER_PROTO_VERSION;
    if (strName == "SPORK_BTX_19_MIN_VERSION_COUNT")            return SPORK_BTX_19_MIN_VERSION_COUNT;
    if (strName == "SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT")   return SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT;
    if (strName == "SPORK_BTX_16_COINBASE_MATURITY_STAGE_3")            return SPORK_BTX_16_COINBASE_MATURITY_STAGE_3;
    if (strName == "SPORK_22_FACTOR_ENFORCEMENT")               return SPORK_22_FACTOR_ENFORCEMENT;

    LogPrint(BCLog::SPORK, "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);
    return -1;
}

std::string CSporkManager::GetSporkNameByID(int nSporkID)
{
    switch (nSporkID) {
        case SPORK_2_INSTANTSEND_ENABLED:               return "SPORK_2_INSTANTSEND_ENABLED";
        case SPORK_3_INSTANTSEND_BLOCK_FILTERING:       return "SPORK_3_INSTANTSEND_BLOCK_FILTERING";
        case SPORK_5_INSTANTSEND_MAX_VALUE:             return "SPORK_5_INSTANTSEND_MAX_VALUE";
        case SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:    return "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT";
        case SPORK_9_SUPERBLOCKS_ENABLED:               return "SPORK_9_SUPERBLOCKS_ENABLED";
        case SPORK_10_UNKNOW:     return "SPORK_10_UNKNOW";
        case SPORK_12_RECONSIDER_BLOCKS:                return "SPORK_12_RECONSIDER_BLOCKS";
        case SPORK_13_OLD_SUPERBLOCK_FLAG:              return "SPORK_13_OLD_SUPERBLOCK_FLAG";
        case SPORK_14_UNKNOW:                           return "SPORK_14_UNKNOW";

        case SPORK_BTX_01_HANDBRAKE_HEIGHT:             return "SPORK_BTX_01_HANDBRAKE_HEIGHT";
        case SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D:      return "SPORK_BTX_01_HANDBRAKE_FORCE_SHA256D";
        case SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT:       return "SPORK_BTX_01_HANDBRAKE_FORCE_SCRYPT";
        case SPORK_BTX_01_HANDBRAKE_FORCE_NIST5:        return "SPORK_BTX_01_HANDBRAKE_FORCE_NIST5";
        case SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z:       return "SPORK_BTX_01_HANDBRAKE_FORCE_LYRA2Z";
        case SPORK_BTX_01_HANDBRAKE_FORCE_X11:          return "SPORK_BTX_01_HANDBRAKE_FORCE_X11";
        case SPORK_BTX_01_HANDBRAKE_FORCE_X16R:         return "SPORK_BTX_01_HANDBRAKE_FORCE_X16R";

        case SPORK_BTX_15_COINBASE_MATURITY_STAGE_2:      return "SPORK_BTX_15_COINBASE_MATURITY_STAGE_2";
        case SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK:      return "SPORK_BTX_02_IGNORE_EXTRA_REWARD_CHECK";
        case SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE:      return "SPORK_BTX_02_IGNORE_EXTRA_REWARD_VALUE";
        case SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT:     return "SPORK_22_MASTERNODE_PAYMENT_ENFORCEMENT";
        case SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING:  return "SPORK_BTX_23_MASTERNODE_PAYMENT_LOW_VOTING";

        case SPORK_BTX_18_MIN_PEER_PROTO_VERSION:         return "SPORK_BTX_18_MIN_PEER_PROTO_VERSION";
        case SPORK_BTX_19_MIN_VERSION_COUNT:              return "SPORK_BTX_19_MIN_VERSION_COUNT";
        case SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT:     return "SPORK_BTX_20_MIN_FOR_WORK_VERSION_COUNT";
        case SPORK_BTX_16_COINBASE_MATURITY_STAGE_3:              return "SPORK_BTX_16_COINBASE_MATURITY_STAGE_3";
        case SPORK_22_FACTOR_ENFORCEMENT:                 return "SPORK_22_FACTOR_ENFORCEMENT";

        default:
            LogPrint(BCLog::SPORK, "CSporkManager::GetSporkNameByID -- Unknown Spork ID %d\n", nSporkID);
            return "Unknown";
    }
}

bool CSporkManager::SetPrivKey(std::string strPrivKey)
{
    CSporkMessage spork;

    spork.Sign(strPrivKey);

    if(spork.CheckSignature()){
        // Test signing successful, proceed
        LogPrintf("CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");
        strMasterPrivKey = strPrivKey;
        return true;
    } else {
        return false;
    }
}

bool CSporkMessage::Sign(std::string strSignKey)
{
    CKey key;
    CPubKey pubkey;
    std::string strError = "";
    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + boost::lexical_cast<std::string>(nValue) + boost::lexical_cast<std::string>(nTimeSigned);

    if(!CMessageSigner::GetKeysFromSecret(strSignKey, key, pubkey)) {
        LogPrintf("CSporkMessage::Sign -- GetKeysFromSecret() failed, invalid spork key %s\n", strSignKey);
        return false;
    }

    if(!CMessageSigner::SignMessage(strMessage, vchSig, key)) {
        LogPrintf("CSporkMessage::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CMessageSigner::VerifyMessage(pubkey, vchSig, strMessage, strError)) {
        LogPrintf("CSporkMessage::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CSporkMessage::CheckSignature()
{
    //note: need to investigate why this is failing
    std::string strError = "";
    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + boost::lexical_cast<std::string>(nValue) + boost::lexical_cast<std::string>(nTimeSigned);
    CPubKey pubkey(ParseHex(Params().SporkPubKey()));

    if(!CMessageSigner::VerifyMessage(pubkey, vchSig, strMessage, strError)) {
        LogPrintf("CSporkMessage::CheckSignature -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CSporkMessage::Relay(CConnman& connman)
{
    CInv inv(MSG_SPORK, GetHash());
    connman.RelayInv(inv);
}
