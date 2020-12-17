// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013 Novacoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015 The Crave developers
// Copyright (c) 2017 XUVCoin developers
// Copyright (c) 2018-2020 Profit Hunters Coin developers

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php


#include "masternodeman.h"
#include "masternode.h"
#include "activemasternode.h"
#include "darksend.h"
#include "core.h"
#include "util.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>


/** Masternode manager */
CMasternodeMan mnodeman;
CCriticalSection cs_process_message;


struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1, const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};


struct CompareValueOnlyMN
{
    bool operator()(const pair<int64_t, CMasternode>& t1, const pair<int64_t, CMasternode>& t2) const
    {
        return t1.first < t2.first;
    }
};


//
// CMasternodeDB
//

CMasternodeDB::CMasternodeDB()
{
    pathMN = GetDataDir(true) / "mncache.dat";
    strMagicMessage = "MasternodeCache";
}


bool CMasternodeDB::Write(const CMasternodeMan& mnodemanToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssMasternodes(SER_DISK, CLIENT_VERSION);
    ssMasternodes << strMagicMessage; // masternode cache file specific magic message
    ssMasternodes << FLATDATA(Params().MessageStart()); // network specific magic number
    ssMasternodes << mnodemanToSave;
    
    uint256 hash = Hash(ssMasternodes.begin(), ssMasternodes.end());
    ssMasternodes << hash;

    // open output file, and associate with CAutoFile
    FILE *file = fopen(pathMN.string().c_str(), "wb");
    CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);

    if (fileout.IsNull())
    {
        return error("%s : ERROR - Failed to open file %s", __FUNCTION__, pathMN.string());
    }

    // Write and commit header, data
    try
    {
        fileout << ssMasternodes;
    }
    catch (std::exception &e)
    {
        return error("%s : ERROR - Serialize or I/O - %s", __FUNCTION__, e.what());
    }

    FileCommit(fileout.Get());

    fileout.fclose();

    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE Written info to mncache.dat  %dms \n", __FUNCTION__, GetTimeMillis() - nStart);
        
        LogPrint("masternode", "%s : OK - %s\n", __FUNCTION__, mnodemanToSave.ToString());
    }

    return true;
}


CMasternodeDB::ReadResult CMasternodeDB::Read(CMasternodeMan& mnodemanToLoad)
{
    int64_t nStart = GetTimeMillis();
    
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathMN.string().c_str(), "rb");

    CAutoFile filein = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    
    if (filein.IsNull())
    {
        error("%s : ERROR - Failed to open file %s", __FUNCTION__, pathMN.string());
        
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
    {
        dataSize = 0;
    }

    vector<unsigned char> vchData;
    vchData.resize(dataSize);

    uint256 hashIn;

    // read data and checksum from file
    try
    {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (std::exception &e)
    {
        error("%s : ERROR - Deserialize or I/O error - %s", __FUNCTION__, e.what());
    
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssMasternodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssMasternodes.begin(), ssMasternodes.end());

    if (hashIn != hashTmp)
    {
        error("%s : ERROR - Checksum mismatch, data corrupted", __FUNCTION__);
    
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];

    std::string strMagicMessageTmp;
    
    try
    {
        // de-serialize file header (masternode cache file specific magic message) and ..

        ssMasternodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp)
        {
            error("%s : ERROR - Invalid masternode cache magic message", __FUNCTION__);
            
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        ssMasternodes >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
        {
            error("%s : ERROR - Invalid network magic number", __FUNCTION__);
            
            return IncorrectMagicNumber;
        }

        // de-serialize address data into one CMnList object
        ssMasternodes >> mnodemanToLoad;
    }
    catch (std::exception &e)
    {
        mnodemanToLoad.Clear();
    
        error("%s : ERROR - Deserialize or I/O error - %s", __FUNCTION__, e.what());
    
        return IncorrectFormat;
    }

    // clean out expired
    mnodemanToLoad.CheckAndRemove();
    
    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE - Loaded info from mncache.dat  %dms \n", __FUNCTION__, GetTimeMillis() - nStart);
    
        LogPrint("masternode", "%s : OK - %s \n", __FUNCTION__, mnodemanToLoad.ToString());
    }

    return Ok;
}


void DumpMasternodes()
{
    int64_t nStart = GetTimeMillis();

    CMasternodeDB mndb;
    CMasternodeMan tempMnodeman;

    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE - Verifying mncache.dat format... \n", __FUNCTION__);
    }

    CMasternodeDB::ReadResult readResult = mndb.Read(tempMnodeman);
    
    // there was an error and it was not an error on file openning => do not proceed
    if (readResult == CMasternodeDB::FileError)
    {
        if (fDebug)
        {
            LogPrint("masternode", "%s : WARNING - Missing masternode list file - mncache.dat, will try to recreate \n", __FUNCTION__);
        }
    }
    else if (readResult != CMasternodeDB::Ok)
    {
        if (fDebug)
        {
            LogPrint("masternode", "%s : ERROR - Reading mncache.dat: ", __FUNCTION__);
        }

        if(readResult == CMasternodeDB::IncorrectFormat)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : WARNING - Magic is ok but data has invalid format, will try to recreate \n", __FUNCTION__);
            }
        }
        else
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - File format is unknown or invalid, please fix it manually \n", __FUNCTION__);
            }

            return;
        }
    }

    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE - Writting info to mncache.dat... \n", __FUNCTION__);
    }

    mndb.Write(mnodeman);

    if (fDebug)
    {
        LogPrint("masternode", "%s : OK - Masternode dump finished  %dms \n", __FUNCTION__, GetTimeMillis() - nStart);
    }
}


CMasternodeMan::CMasternodeMan()
{
    nDsqCount = 0;
}


bool CMasternodeMan::Add(CMasternode &mn)
{
    LOCK(cs);

    /*
    if (!mn.IsEnabled())
    {
        return false;
    }
    */
    
    // Check IP is not already found in the list
    CMasternode *pmn1 = Find(mn.addr);

    if (pmn1 != NULL)
    {
        if (fDebug)
        {
            LogPrint("masternode", "%s : WARNING - Duplicate IP found for masternode %s - %i skipping \n", __FUNCTION__, mn.addr.ToStringIPPort().c_str(), size() + 1);
        }

        return false;
    }

    // Check VIN is not already found in the list
    CMasternode *pmn2 = Find(mn.vin);

    if (pmn2 == NULL)
    {
        if (fDebug)
        {
            LogPrint("masternode", "%s : OK - Adding new masternode %s - %i now \n", __FUNCTION__, mn.addr.ToStringIPPort().c_str(), size() + 1);
        }

        // Add to Masternode List
        vMasternodes.push_back(mn);
        
        return true;
    }

    // Fail: Error
    return false;
}


void CMasternodeMan::AskForMN(CNode* pnode, CTxIn &vin)
{
    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
    
    if (i != mWeAskedForMasternodeListEntry.end())
    {
        int64_t t = (*i).second;
        
        if (GetTime() < t)
        {
            // we've asked recently
            return;
        }
    }

    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE - Asking node for missing entry, vin: %s \n", __FUNCTION__, vin.ToString());
    }

    // ask for the mnb info once from the node that sent mnp
    pnode->PushMessage("dseg", vin);
    
    int64_t askAgain = GetTime() + MASTERNODE_MIN_DSEEP_SECONDS;
    
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}


void CMasternodeMan::Check()
{
    LOCK(cs);

    for(CMasternode& mn: vMasternodes)
    {
        mn.Check();
    }
}


void CMasternodeMan::CheckAndRemove()
{
    LOCK(cs);

    Check();

    vector<CMasternode>::iterator it = vMasternodes.begin();
    
    while(it != vMasternodes.end())
    {
        if( (*it).activeState == CMasternode::MASTERNODE_REMOVE
            || (*it).activeState == CMasternode::MASTERNODE_VIN_SPENT
            || (*it).activeState != CMasternode::MASTERNODE_UNREACHABLE
            || (*it).activeState != CMasternode::MASTERNODE_PEER_ERROR
            || (*it).protocolVersion < nMasternodeMinProtocol
            || Count((*it).addr) > 1
            )
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : WARNING - Removing inactive masternode %s - %i now \n", __FUNCTION__, (*it).addr.ToStringIPPort().c_str(), size() - 1);
            }

            it = vMasternodes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // check who's asked for the masternode list
    map<CNetAddr, int64_t>::iterator it1 = mAskedUsForMasternodeList.begin();

    while(it1 != mAskedUsForMasternodeList.end())
    {
        if((*it1).second < GetTime())
        {
            mAskedUsForMasternodeList.erase(it1++);
        }
        else
        {
            ++it1;
        }
    }

    // check who we asked for the masternode list
    it1 = mWeAskedForMasternodeList.begin();

    while(it1 != mWeAskedForMasternodeList.end())
    {
        if((*it1).second < GetTime())
        {
            mWeAskedForMasternodeList.erase(it1++);
        }
        else
        {
            ++it1;
        }
    }

    // check which masternodes we've asked for
    map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();

    while(it2 != mWeAskedForMasternodeListEntry.end())
    {
        if((*it2).second < GetTime())
        {
            mWeAskedForMasternodeListEntry.erase(it2++);
        }
        else
        {
            ++it2;
        }
    }

}


void CMasternodeMan::Clear()
{
    LOCK(cs);

    vMasternodes.clear();
    
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    
    nDsqCount = 0;
}


int CMasternodeMan::CountEnabled(int protocolVersion)
{
    int i = 0;

    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;

    for(CMasternode& mn: vMasternodes)
    {
        mn.Check();

        if(mn.protocolVersion < protocolVersion
            || !mn.IsEnabled())
        {
            continue;
        }

        i++;
    }

    return i;
}


int CMasternodeMan::CountMasternodesAboveProtocol(int protocolVersion)
{
    int i = 0;

    for(CMasternode& mn: vMasternodes)
    {
        mn.Check();

        if(mn.protocolVersion < protocolVersion
            || !mn.IsEnabled())
        {
            continue;
        }

        i++;
    }

    return i;
}


void CMasternodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    std::map<CNetAddr, int64_t>::iterator it = mWeAskedForMasternodeList.find(pnode->addr);

    if (it != mWeAskedForMasternodeList.end())
    {
        if (GetTime() < (*it).second)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : WARNING - We already asked %s for the list; skipping... \n", __FUNCTION__, pnode->addr.ToStringIPPort());
            }

            return;
        }
    }

    pnode->PushMessage("dseg", CTxIn());
    
    int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
    
    mWeAskedForMasternodeList[pnode->addr] = askAgain;
}


CMasternode* CMasternodeMan::Find(const CScript& payee)
{
    LOCK(cs);

    CScript payee2;

    for(CMasternode& mn: vMasternodes)
    {
        payee2 = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

        if (payee2 == payee)
        {
            return &mn;
        }
    }

    return NULL;
}


CMasternode *CMasternodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    for(CMasternode& mn: vMasternodes)
    {
        if(mn.vin.prevout == vin.prevout)
        {
            return &mn;
        }
    }

    return NULL;
}


CMasternode *CMasternodeMan::Find(const CService &addr)
{
    LOCK(cs);

    for(CMasternode& mn: vMasternodes)
    {
        // Find IP Address but ignore Port
        if(mn.addr.ToString() == addr.ToString())
        {
            return &mn;
        }
    }

    return NULL;
}


CMasternode* CMasternodeMan::FindOldestNotInVec(const std::vector<CTxIn> &vVins, int nMinimumAge)
{
    LOCK(cs);

    CMasternode *pOldestMasternode = NULL;

    for(CMasternode &mn: vMasternodes)
    {   
        mn.Check();

        if(!mn.IsEnabled())
        {
            continue;
        }

        if(mn.GetMasternodeInputAge() < nMinimumAge) 
        {
            continue;
        }

        bool found = false;

        for(const CTxIn& vin: vVins)
        {
            if(mn.vin.prevout == vin.prevout)
            {   
                found = true;

                break;
            } 
        }

        if(found)
        {
            continue;
        }

        if(pOldestMasternode == NULL
            || pOldestMasternode->SecondsSincePayment() < mn.SecondsSincePayment())
        {
            pOldestMasternode = &mn;
        }
    }

    return pOldestMasternode;
}


CMasternode *CMasternodeMan::FindRandom()
{
    LOCK(cs);

    if(size() == 0)
    {
        return NULL;
    }

    return &vMasternodes[GetRandInt(vMasternodes.size())];
}


CMasternode *CMasternodeMan::Find(const CPubKey &pubKeyMasternode)
{
    LOCK(cs);

    for(CMasternode& mn: vMasternodes)
    {
        if(mn.pubKeyMasternode == pubKeyMasternode)
        {
            return &mn;
        }
    }

    return NULL;
}


CMasternode *CMasternodeMan::FindRandomNotInVec(std::vector<CTxIn> &vecToExclude, int protocolVersion)
{
    LOCK(cs);

    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;

    int nCountEnabled = CountEnabled(protocolVersion);
    
    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE - nCountEnabled - vecToExclude.size() %d \n", __FUNCTION__, nCountEnabled - vecToExclude.size());
    }

    if(nCountEnabled - vecToExclude.size() < 1)
    {
        return NULL;
    }

    int rand = GetRandInt(nCountEnabled - vecToExclude.size());

    if (fDebug)
    {
        LogPrint("masternode", "%s : NOTICE - rand %d \n", __FUNCTION__, rand);
    }

    bool found;

    for(CMasternode &mn: vMasternodes) 
    {
        if(mn.protocolVersion < protocolVersion
            || !mn.IsEnabled())
        {
            continue;
        }

        found = false;

        for(CTxIn &usedVin: vecToExclude)
        {
            if(mn.vin.prevout == usedVin.prevout)
            {
                found = true;
            
                break;
            }
        }

        if(found)
        {
            continue;
        }
        
        if(--rand < 1)
        {
            return &mn;
        }
    }

    return NULL;
}


int *CMasternodeMan::Count(const CService &addr)
{
    LOCK(cs);

    int nCount = 0;

    for(CMasternode& mn: vMasternodes)
    {
        // Find number of occurances IP Address but ignore Port
        if(mn.addr.ToString() == addr.ToString())
        {
            nCount++;
        }
    }

    return nCount;
}

CMasternode* CMasternodeMan::GetCurrentMasterNode(int mod, int64_t nBlockHeight, int minProtocol)
{
    unsigned int score = 0;

    CMasternode* winner = NULL;

    // scan for winner
    for(CMasternode& mn: vMasternodes)
    {
        mn.Check();

        if(mn.protocolVersion < minProtocol
            || !mn.IsEnabled())
        {
            continue;
        }

        // calculate the score for each masternode
        uint256 n = mn.CalculateScore(mod, nBlockHeight);

        unsigned int n2 = 0;

        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if(n2 > score)
        {
            score = n2;
            winner = &mn;
        }
    }

    return winner;
}


int CMasternodeMan::GetMasternodeRank(const CTxIn& vin, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    //make sure we know about this block
    uint256 hash = 0;

    if(!GetBlockHash(hash, nBlockHeight))
    {
        return -1;
    }

    // scan for winner
    for(CMasternode& mn: vMasternodes)
    {

        if(mn.protocolVersion < minProtocol)
        {
            continue;
        }

        if(fOnlyActive)
        {
            mn.Check();

            if(!mn.IsEnabled())
            {
                continue;
            }
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);

        unsigned int n2 = 0;

        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    int rank = 0;

    for(PAIRTYPE(unsigned int, CTxIn)& s: vecMasternodeScores)
    {
        rank++;

        if(s.second == vin)
        {
            return rank;
        }
    }

    return -1;
}


std::vector<pair<int, CMasternode> > CMasternodeMan::GetMasternodeRanks(int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<unsigned int, CMasternode> > vecMasternodeScores;
    std::vector<pair<int, CMasternode> > vecMasternodeRanks;

    //make sure we know about this block
    uint256 hash = 0;

    if(!GetBlockHash(hash, nBlockHeight)) 
    {
        return vecMasternodeRanks;
    }

    // scan for winner
    for(CMasternode& mn: vMasternodes)
    {

        mn.Check();

        if(mn.protocolVersion < minProtocol) 
        {
            continue;
        }

        if(!mn.IsEnabled())
        {
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);

        unsigned int n2 = 0;

        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnlyMN());

    int rank = 0;

    for(PAIRTYPE(unsigned int, CMasternode)& s: vecMasternodeScores)
    {
        rank++;
    
        vecMasternodeRanks.push_back(make_pair(rank, s.second));
    }

    return vecMasternodeRanks;
}


CMasternode* CMasternodeMan::GetMasternodeByRank(int nRank, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    // scan for winner
    for(CMasternode& mn: vMasternodes)
    {

        if(mn.protocolVersion < minProtocol) 
        {
            continue;
        }

        if(fOnlyActive)
        {
            mn.Check();

            if(!mn.IsEnabled())
            {
                continue;
            }
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);

        unsigned int n2 = 0;

        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    int rank = 0;

    for(PAIRTYPE(unsigned int, CTxIn)& s: vecMasternodeScores)
    {
        rank++;

        if(rank == nRank)
        {
            return Find(s.second);
        }
    }

    return NULL;
}


void CMasternodeMan::ProcessMasternodeConnections()
{
    LOCK(cs_vNodes);

    if(!darkSendPool.pSubmittedToMasternode)
    {
        return;
    }
    
    for(CNode* pnode: vNodes)
    {
        if(darkSendPool.pSubmittedToMasternode->addr == pnode->addr)
        {
            continue;
        }

        if(pnode->fDarkSendMaster)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : OK - Closing masternode connection %s \n", __FUNCTION__, pnode->addr.ToStringIPPort().c_str());
            }

            pnode->CloseSocketDisconnect();
        }
    }
}


void CMasternodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

    //Normally would disable functionality, NEED this enabled for staking.
    //if(fLiteMode) return;

    if(!darkSendPool.IsBlockchainSynced())
    {
        return;
    }

    LOCK(cs_process_message);

    if (strCommand == "dsee")
    {
        //DarkSend Election Entry

        CTxIn vin;
        
        CService addr;
        
        CPubKey pubKeyCollateralAddress;
        CPubKey pubKeyMasternode;
        
        vector<unsigned char> vchSig;
        
        int64_t sigTime;
        
        int count;
        int current;
        
        int64_t lastUpdated;
        
        int protocolVersion;
        
        std::string strMessage;
        
        CScript rewardAddress = CScript();
        
        int rewardPercentage = 0;
        
        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubKeyCollateralAddress >> pubKeyMasternode >> count >> current >> lastUpdated >> protocolVersion;

        //Invalid nodes check
        if (sigTime < 1511159400)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Bad packet \n", __FUNCTION__);
            }

            return;
        }
        
        if (sigTime > lastUpdated)
        {
            LogPrint("masternode", "%s : ERROR - Bad node entry \n", __FUNCTION__);
            
            return;
        }
        
        if (addr.GetPort() == 0)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Bad port \n", __FUNCTION__);
            }

            return;
        }
        
        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Signature rejected, too far into the future %s \n", __FUNCTION__, vin.ToString().c_str());
            }

            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();

        //if(RegTest()) isLocal = false;

        std::string vchPubKeyCollateralAddress(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
        std::string vchPubKeyMasternode(pubKeyMasternode.begin(), pubKeyMasternode.end());

        strMessage = addr.ToStringIPPort()
            + boost::lexical_cast<std::string>(sigTime)
            + vchPubKeyCollateralAddress
            + vchPubKeyMasternode
            + boost::lexical_cast<std::string>(protocolVersion);    

        if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Ignoring outdated masternode %s protocol version %d \n", __FUNCTION__, vin.ToString().c_str(), protocolVersion);
            }

            return;
        }

        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubKeyCollateralAddress.GetID());

        if(pubkeyScript.size() != 25)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - PubkeySCRIPT the wrong size \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);

            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubKeyMasternode.GetID());

        if(pubkeyScript2.size() != 25)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - pubKeyMasternode the wrong size \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);

            return;
        }

        if(!vin.scriptSig.empty())
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Ignore Not Empty ScriptSig %s \n", __FUNCTION__, vin.ToString().c_str());
            }

            return;
        }

        std::string errorMessage = "";

        if(!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, errorMessage))
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Got bad masternode address signature \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);

            return;
        }

        //search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasternode* pmn = this->Find(vin);

        // if we are a masternode but with undefined vin and this dsee is ours (matches our Masternode privkey) then just skip this part
        if(pmn != NULL
            && !(fMasterNode
            && activeMasternode.vin == CTxIn()
            && pubKeyMasternode == activeMasternode.pubKeyMasternode))
        {
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubKeyCollateralAddress = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1
                && pmn->pubKeyCollateralAddress == pubKeyCollateralAddress
                && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS))
            {
                pmn->UpdateLastSeen();

                if(pmn->sigTime < sigTime)
                {
                    // use this as a peer
                    addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);

                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : OK - Got updated entry for %s \n", __FUNCTION__, addr.ToStringIPPort().c_str());
                    }

                    pmn->pubKeyMasternode = pubKeyMasternode;
                    pmn->sigTime = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;
                    pmn->Check();
                    pmn->isOldNode = true;
                    
                    if(pmn->IsEnabled())
                    {
                        mnodeman.RelayOldMasternodeEntry(vin, addr, vchSig, sigTime, pubKeyCollateralAddress, pubKeyMasternode, count, current, lastUpdated, protocolVersion);
                    }
                }
            }

            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress))
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Got mismatched pubkey and vin \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);
            
            return;
        }

        if (fDebug)
        {
            LogPrint("masternode", "%s : NOTICE - Got NEW masternode entry %s \n", __FUNCTION__, addr.ToStringIPPort().c_str());
        }

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()

        CValidationState state;
        
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
        
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        bool fAcceptable = false;

        // Global Namespace Start
        {
            TRY_LOCK(cs_main, lockMain);
            
            if(!lockMain)
            {
                return;
            }
            
            fAcceptable = AcceptableInputs(mempool, tx, false, NULL);
        }
        // Global Namespace End

        if(fAcceptable)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : OK - Accepted masternode entry %i %i \n", __FUNCTION__, count, current);
            }

            if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS)
            {
                if (fDebug)
                {
                    LogPrint("masternode", "%s : ERROR - Input must have least %d confirmations \n", __FUNCTION__, MASTERNODE_MIN_CONFIRMATIONS);
                }

                Misbehaving(pfrom->GetId(), 20);
                
                return;
            }

            // verify that sig time is legit in past
            // should be at least not earlier than block when 10000 PHC tx got MASTERNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;
            
            GetTransaction(vin.prevout.hash, tx, hashBlock);

            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            
            if (mi != mapBlockIndex.end() && (*mi).second)
            {
                // block for 10000 PHC tx -> 1 confirmation
                CBlockIndex* pMNIndex = (*mi).second;
                CBlockIndex* pConfIndex = FindBlockByHeight((pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1)); // block where tx got MASTERNODE_MIN_CONFIRMATIONS
                
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : ERROR - Bad sigTime %d for masternode %20s %105s (%i conf block is at %d) \n", __FUNCTION__, sigTime, addr.ToStringIPPort(), vin.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    }

                    return;
                }
            }

            // add our masternode
            CMasternode mn(addr, vin, pubKeyCollateralAddress, vchSig, sigTime, pubKeyMasternode, protocolVersion, rewardAddress, rewardPercentage);
            mn.UpdateLastSeen(lastUpdated);

            // use this as a peer
            if (!CheckNode((CAddress)addr))
            {
                mn.ChangePortStatus(false);
            }
            else
            {
                addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
            }
            
            mn.ChangeNodeStatus(true);
            this->Add(mn);

            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubKeyMasternode == activeMasternode.pubKeyMasternode
                && protocolVersion == PROTOCOL_VERSION)
            {
                activeMasternode.EnableHotColdMasterNode(vin, addr);
            }

            if(count == -1 && !isLocal)
            {
                mnodeman.RelayOldMasternodeEntry(vin, addr, vchSig, sigTime, pubKeyCollateralAddress, pubKeyMasternode, count, current, lastUpdated, protocolVersion);
            }
        }
        else
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : WARNING - Rejected masternode entry %s \n", __FUNCTION__, addr.ToStringIPPort().c_str());
            }

            int nDoS = 0;

            if (state.IsInvalid(nDoS))
            {
                if (fDebug)
                {
                    LogPrint("masternode", "%s : ERROR - %s from %s %s was not accepted into the memory pool\n", __FUNCTION__, tx.GetHash().ToString().c_str(), pfrom->addr.ToStringIPPort().c_str(), pfrom->cleanSubVer.c_str());
                }

                if (nDoS > 0)
                {
                    Misbehaving(pfrom->GetId(), nDoS);
                }
            }
        }
    }
    else if (strCommand == "dsee+")
    {
        //DarkSend Election Entry+

        CTxIn vin;
        
        CService addr;
        
        CPubKey pubKeyCollateralAddress;
        CPubKey pubKeyMasternode;
        
        vector<unsigned char> vchSig;
        
        int64_t sigTime;
        
        int count;
        int current;
        
        int64_t lastUpdated;
        
        int protocolVersion;
        
        CScript rewardAddress;
        
        int rewardPercentage;
        
        std::string strMessage;

        // 70047 and greater
        vRecv >> vin
                >> addr
                >> vchSig
                >> sigTime
                >> pubKeyCollateralAddress
                >> pubKeyMasternode
                >> count
                >> current
                >> lastUpdated
                >> protocolVersion
                >> rewardAddress
                >> rewardPercentage;

        //Invalid nodes check
        if (sigTime < 1511159400)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Bad packet \n", __FUNCTION__);
            }

            return;
        }
        
        if (sigTime > lastUpdated)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Bad node entry \n", __FUNCTION__);
            }

            return;
        }
        
        if (addr.GetPort() == 0)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Bad port \n", __FUNCTION__);
            }

            return;
        }
        
        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Signature rejected, too far into the future %s \n", __FUNCTION__, vin.ToString().c_str());
            }

            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();

        //if(RegTest()) isLocal = false;

        std::string vchPubKeyCollateralAddress(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
        std::string vchPubKeyMasternode(pubKeyMasternode.begin(), pubKeyMasternode.end());

        strMessage = addr.ToStringIPPort()
                    + boost::lexical_cast<std::string>(sigTime)
                    + vchPubKeyCollateralAddress
                    + vchPubKeyMasternode
                    + boost::lexical_cast<std::string>(protocolVersion) 
                    + rewardAddress.ToString()
                    + boost::lexical_cast<std::string>(rewardPercentage);
        
        if(rewardPercentage < 0
            || rewardPercentage > 100)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Reward percentage out of range %d \n", __FUNCTION__, rewardPercentage);
            }

            return;
        }

        if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Ignoring outdated masternode %s protocol version %d \n", __FUNCTION__, vin.ToString().c_str(), protocolVersion);
            }

            return;
        }

        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubKeyCollateralAddress.GetID());

        if(pubkeyScript.size() != 25)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Pubkey Collateral Address the wrong size \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);
            
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubKeyMasternode.GetID());

        if(pubkeyScript2.size() != 25)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - pubKeyMasternode the wrong size \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);
            
            return;
        }

        if(!vin.scriptSig.empty())
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Ignore Not Empty ScriptSig %s \n", __FUNCTION__, vin.ToString().c_str());
            }

            return;
        }

        std::string errorMessage = "";

        if(!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, errorMessage))
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Got bad masternode address signature \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);
            
            return;
        }

        //search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasternode* pmn = this->Find(vin);

        // if we are a masternode but with undefined vin and this dsee is ours (matches our Masternode privkey) then just skip this part
        if(pmn != NULL
            && !(fMasterNode
            && activeMasternode.vin == CTxIn()
            && pubKeyMasternode == activeMasternode.pubKeyMasternode))
        {
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubKeyCollateralAddress = pubKeyCollateralAddress, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1
                && pmn->pubKeyCollateralAddress == pubKeyCollateralAddress
                && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS))
            {
                pmn->UpdateLastSeen();

                // take the newest entry
                if(pmn->sigTime < sigTime)
                {
                    // use this as a peer
                    addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);

                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : OK - Got updated entry for %s \n", __FUNCTION__, addr.ToStringIPPort().c_str());
                    }

                    pmn->pubKeyMasternode = pubKeyMasternode;
                    pmn->sigTime = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;
                    pmn->rewardAddress = rewardAddress;
                    pmn->rewardPercentage = rewardPercentage;                    
                    pmn->Check();
                    pmn->isOldNode = false;

                    if(pmn->IsEnabled())
                    {
                        mnodeman.RelayMasternodeEntry(vin, addr, vchSig, sigTime, pubKeyCollateralAddress, pubKeyMasternode, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage);
                    }
                }
            }

            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress))
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : WARNING - Got mismatched pubKeyCollateralAddress and vin \n", __FUNCTION__);
            }

            Misbehaving(pfrom->GetId(), 100);

            return;
        }

        if (fDebug)
        {
            LogPrint("masternode", "%s : OK - Got NEW masternode entry %s \n", __FUNCTION__, addr.ToStringIPPort().c_str());
        }

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()

        CValidationState state;
        
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
        
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        
        bool fAcceptable = false;

        // Globl Namespace Start
        {
            TRY_LOCK(cs_main, lockMain);
            
            if(!lockMain)
            {
                return;
            }
            
            fAcceptable = AcceptableInputs(mempool, tx, false, NULL);
        }
        // Global Namespace End

        if(fAcceptable)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : OK - Accepted masternode entry %i %i \n", __FUNCTION__, count, current);
            }

            if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS)
            {
                if (fDebug)
                {
                    LogPrint("masternode", "%s : ERROR - Input must have least %d confirmations \n", __FUNCTION__, MASTERNODE_MIN_CONFIRMATIONS);
                }

                Misbehaving(pfrom->GetId(), 20);
                
                return;
            }

            // verify that sig time is legit in past
            // should be at least not earlier than block when 10000 PHC tx got MASTERNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;

            GetTransaction(vin.prevout.hash, tx, hashBlock);

            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);

            if (mi != mapBlockIndex.end()
                && (*mi).second)
            {
                // block for 10000 PHC tx -> 1 confirmation
                CBlockIndex* pMNIndex = (*mi).second;

                // block where tx got MASTERNODE_MIN_CONFIRMATIONS
                CBlockIndex* pConfIndex = FindBlockByHeight((pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1));
                
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : ERROR - Bad sigTime %d for masternode %20s %105s (%i conf block is at %d) \n", __FUNCTION__, sigTime, addr.ToStringIPPort(), vin.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    }

                    return;
                }
            }


            //doesn't support multisig addresses
            if(rewardAddress.IsPayToScriptHash())
            {
                rewardAddress = CScript();
                rewardPercentage = 0;
            }

            // add our masternode
            CMasternode mn(addr, vin, pubKeyCollateralAddress, vchSig, sigTime, pubKeyMasternode, protocolVersion, rewardAddress, rewardPercentage);

            mn.UpdateLastSeen(lastUpdated);

            if (!CheckNode((CAddress)addr))
            {
                mn.ChangePortStatus(false);
            }
            else
            {
                // use this as a peer
                addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);
            }
            
            mn.ChangeNodeStatus(false);
            this->Add(mn);
            
            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubKeyMasternode == activeMasternode.pubKeyMasternode
                && protocolVersion == PROTOCOL_VERSION)
            {
                activeMasternode.EnableHotColdMasterNode(vin, addr);
            }

            if(count == -1 && !isLocal)
            {
                mnodeman.RelayMasternodeEntry(vin, addr, vchSig, sigTime, pubKeyCollateralAddress, pubKeyMasternode, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage);
            }
        }
        else
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : WARNING - Rejected masternode entry %s \n", __FUNCTION__, addr.ToStringIPPort().c_str());
            }

            int nDoS = 0;

            if (state.IsInvalid(nDoS))
            {
                if (fDebug)
                {
                    LogPrint("masternode", "%s : ERROR - %s from %s %s was not accepted into the memory pool \n", __FUNCTION__, tx.GetHash().ToString().c_str(), pfrom->addr.ToStringIPPort().c_str(), pfrom->cleanSubVer.c_str());
                }

                if (nDoS > 0)
                {
                    Misbehaving(pfrom->GetId(), nDoS);
                }
            }
        }
    }
    else if (strCommand == "dseep")
    {
        //DarkSend Election Entry Ping

        CTxIn vin;
        
        vector<unsigned char> vchSig;
        
        int64_t sigTime;
       
        bool stop;
        
        vRecv >> vin >> vchSig >> sigTime >> stop;

        if (fDebug)
        {
            LogPrint("masternode", "%s : NOTICE - Received: vin: %s sigTime: %lld stop: %s \n", __FUNCTION__, vin.ToString().c_str(), sigTime, stop ? "true" : "false");
        }

        if (sigTime > GetAdjustedTime() + 60 * 60)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Signature rejected, too far into the future %s \n", __FUNCTION__, vin.ToString().c_str());
            }

            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : ERROR - Signature rejected, too far into the past %s - %d %d \n", __FUNCTION__, vin.ToString().c_str(), sigTime, GetAdjustedTime());
            }

            return;
        }

        // see if we have this masternode
        CMasternode* pmn = this->Find(vin);

        if(pmn != NULL
            && pmn->protocolVersion >= MIN_POOL_PEER_PROTO_VERSION)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : NOTICE - Found corresponding mn for vin: %s \n", __FUNCTION__, vin.ToString().c_str());
            }

            // take this only if it's newer
            if(pmn->lastDseep < sigTime)
            {
                std::string strMessage = pmn->addr.ToStringIPPort()
                                        + boost::lexical_cast<std::string>(sigTime)
                                        + boost::lexical_cast<std::string>(stop);

                std::string errorMessage = "";

                if(!darkSendSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage))
                {
                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : WARNING - Got bad masternode address signature %s \n", __FUNCTION__, vin.ToString().c_str());
                    }

                    Misbehaving(pfrom->GetId(), 100);
                    
                    return;
                }

                pmn->lastDseep = sigTime;

                if(!pmn->UpdatedWithin(MASTERNODE_MIN_DSEEP_SECONDS))
                {
                    if(stop)
                    {
                        pmn->Disable();
                    }
                    else
                    {
                        pmn->UpdateLastSeen();
                        pmn->Check();

                        if(!pmn->IsEnabled())
                        {
                            return;
                        }
                    }
                    mnodeman.RelayMasternodeEntryPing(vin, vchSig, sigTime, stop);
                }
            }

            return;
        }

        if (fDebug)
        {
            LogPrint("masternode", "%s : ERROR - Couldn't find masternode entry %s \n", __FUNCTION__, vin.ToString().c_str());
        }

        std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
        
        if (i != mWeAskedForMasternodeListEntry.end())
        {
            int64_t t = (*i).second;
            
            if (GetTime() < t)
            {
                // we've asked recently
                return;
            }
        }

        // ask for the dsee info once from the node that sent dseep

        if (fDebug)
        {
            LogPrint("masternode", "%s : WARNING - Asking source node for missing entry %s \n", __FUNCTION__, vin.ToString().c_str());
        }

        pfrom->PushMessage("dseg", vin);
        
        int64_t askAgain = GetTime()+ MASTERNODE_MIN_DSEEP_SECONDS;
        
        mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;

    }
    else if (strCommand == "mvote")
    { //Masternode Vote

        CTxIn vin;
        vector<unsigned char> vchSig;

        int nVote;

        vRecv >> vin >> vchSig >> nVote;

        // see if we have this Masternode
        CMasternode* pmn = this->Find(vin);

        if(pmn != NULL)
        {
            if((GetAdjustedTime() - pmn->lastVote) > (60*60))
            {
                std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nVote);

                std::string errorMessage = "";

                if(!darkSendSigner.VerifyMessage(pmn->pubKeyMasternode, vchSig, strMessage, errorMessage))
                {
                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : ERROR - Mvote Got bad Masternode address signature %s \n", __FUNCTION__, vin.ToString().c_str());
                    }

                    return;
                }

                pmn->nVote = nVote;
                pmn->lastVote = GetAdjustedTime();

                //send to all peers
                LOCK(cs_vNodes);

                for(CNode* pnode: vNodes)
                {
                    pnode->PushMessage("mvote", vin, vchSig, nVote);
                }
            }

            return;
        }

    }
    else if (strCommand == "dseg")
    {
        //Get masternode list or specific entry

        CTxIn vin;
        vRecv >> vin;

        if(vin == CTxIn())
        { //only should ask for this once

            //local network
            if(!pfrom->addr.IsRFC1918()
                && Params().NetworkID() == CChainParams::MAIN)
            {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForMasternodeList.find(pfrom->addr);

                if (i != mAskedUsForMasternodeList.end())
                {
                    int64_t t = (*i).second;

                    if (GetTime() < t)
                    {
                        
                        Misbehaving(pfrom->GetId(), 34);
                        
                        if (fDebug)
                        {
                            LogPrint("masternode", "%s : ERROR - Peer already asked me for the list \n", __FUNCTION__);
                        }

                        return;
                    }
                }

                int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
                
                mAskedUsForMasternodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int count = this->size();
        int i = 0;

        for(CMasternode& mn: vMasternodes)
        {

            if(mn.addr.IsRFC1918())
            {
                //local network
                continue;
            }

            if(mn.IsEnabled())
            {
                if (fDebug)
                {
                    LogPrint("masternode", "%s : NOTICE - Sending masternode entry - %s \n", __FUNCTION__, mn.addr.ToStringIPPort().c_str());
                }

                if(vin == CTxIn())
                {
                    if (mn.isOldNode)
                    {
                        pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubKeyCollateralAddress, mn.pubKeyMasternode, count, i, mn.lastTimeSeen, mn.protocolVersion);
                    }
                    else
                    {
                        pfrom->PushMessage("dsee+", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubKeyCollateralAddress, mn.pubKeyMasternode, count, i, mn.lastTimeSeen, mn.protocolVersion, mn.rewardAddress, mn.rewardPercentage);
                    }
                }
                else if (vin == mn.vin)
                {
                    if (mn.isOldNode)
                    {
                        pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubKeyCollateralAddress, mn.pubKeyMasternode, count, i, mn.lastTimeSeen, mn.protocolVersion);
                    }
                    else
                    {
                        pfrom->PushMessage("dsee+", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubKeyCollateralAddress, mn.pubKeyMasternode, count, i, mn.lastTimeSeen, mn.protocolVersion, mn.rewardAddress, mn.rewardPercentage);
                    }

                    if (fDebug)
                    {
                        LogPrint("masternode", "%s : NOTICE - Sent 1 masternode entries to %s \n", __FUNCTION__, pfrom->addr.ToStringIPPort().c_str());
                    }

                    return;
                }

                i++;
            }
        }

        if (fDebug)
        {
            LogPrint("masternode", "%s : OK - Sent %d masternode entries to %s \n", __FUNCTION__, i, pfrom->addr.ToStringIPPort().c_str());
        }
    }

}


void CMasternodeMan::RelayOldMasternodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubKeyCollateralAddress, const CPubKey pubKeyMasternode, const int count, const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);

    for(CNode* pnode: vNodes)
    {
        pnode->PushMessage("dsee", vin, addr, vchSig, nNow, pubKeyCollateralAddress, pubKeyMasternode, count, current, lastUpdated, protocolVersion);
    }
}


void CMasternodeMan::RelayMasternodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubKeyCollateralAddress, const CPubKey pubKeyMasternode, const int count, const int current, const int64_t lastUpdated, const int protocolVersion, CScript rewardAddress, int rewardPercentage)
{
    LOCK(cs_vNodes);

    for(CNode* pnode: vNodes)
    {
        pnode->PushMessage("dsee+", vin, addr, vchSig, nNow, pubKeyCollateralAddress, pubKeyMasternode, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage);
    }
}


void CMasternodeMan::RelayMasternodeEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);

    for(CNode* pnode: vNodes)
    {
        pnode->PushMessage("dseep", vin, vchSig, nNow, stop);
    }
}


void CMasternodeMan::Remove(CTxIn vin)
{
    LOCK(cs);

    vector<CMasternode>::iterator it = vMasternodes.begin();

    while(it != vMasternodes.end())
    {
        if((*it).vin == vin)
        {
            if (fDebug)
            {
                LogPrint("masternode", "%s : OK - Removing Masternode %s - %i now \n", __FUNCTION__, (*it).addr.ToStringIPPort().c_str(), size() - 1);
            }
            
            vMasternodes.erase(it);
            
            break;
        }
        else
        {
            ++it;
        }
    }
}


std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "masternodes: " << (int)vMasternodes.size() <<
            ", peers who asked us for masternode list: " << (int)mAskedUsForMasternodeList.size() <<
            ", peers we asked for masternode list: " << (int)mWeAskedForMasternodeList.size() <<
            ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}
