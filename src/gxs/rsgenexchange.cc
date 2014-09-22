
/*
 * libretroshare/src/gxs: rsgenexchange.cc
 *
 * RetroShare Gxs exchange interface.
 *
 * Copyright 2012-2012 by Christopher Evi-Parker, Robert Fernie
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License Version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA.
 *
 * Please report all bugs and problems to "retroshare@lunamutt.com".
 *
 */

#include <unistd.h>

#include "pqi/pqihash.h"
#include "rsgenexchange.h"
#include "gxssecurity.h"
#include "util/contentvalue.h"
#include "retroshare/rsgxsflags.h"
#include "retroshare/rsgxscircles.h"
#include "rsgixs.h"
#include "rsgxsutil.h"


#define PUB_GRP_MASK     0x000f
#define RESTR_GRP_MASK   0x00f0
#define PRIV_GRP_MASK    0x0f00
#define GRP_OPTIONS_MASK 0xf000

#define PUB_GRP_OFFSET        0
#define RESTR_GRP_OFFSET      8
#define PRIV_GRP_OFFSET      16
#define GRP_OPTIONS_OFFSET   24

#define GXS_MASK "GXS_MASK_HACK"

//#define GEN_EXCH_DEBUG	1

#define MSG_CLEANUP_PERIOD 60*5 // 5 minutes
#define INTEGRITY_CHECK_PERIOD 60*30 //  30 minutes

RsGenExchange::RsGenExchange(RsGeneralDataService *gds, RsNetworkExchangeService *ns,
                             RsSerialType *serviceSerialiser, uint16_t servType, RsGixs* gixs,
                             uint32_t authenPolicy, uint32_t messageStorePeriod)
  : mGenMtx("GenExchange"),
    mDataStore(gds),
    mNetService(ns),
    mSerialiser(serviceSerialiser),
  mServType(servType),
  mGixs(gixs),
  mAuthenPolicy(authenPolicy),
  MESSAGE_STORE_PERIOD(messageStorePeriod),
  mCleaning(false),
  mLastClean(time(NULL)),
  mMsgCleanUp(NULL),
  mChecking(false),
  mLastCheck(time(NULL)),
  mIntegrityCheck(NULL),
  CREATE_FAIL(0),
  CREATE_SUCCESS(1),
  CREATE_FAIL_TRY_LATER(2),
  SIGN_MAX_ATTEMPTS(5),
  SIGN_FAIL(0),
  SIGN_SUCCESS(1),
  SIGN_FAIL_TRY_LATER(2),
  VALIDATE_FAIL(0),
  VALIDATE_SUCCESS(1),
  VALIDATE_FAIL_TRY_LATER(2),
  VALIDATE_MAX_ATTEMPTS(5)
{

    mDataAccess = new RsGxsDataAccess(gds);

}

#ifdef TO_BE_DELETED_IF_NOT_USEFUL
// This class has been tested so as to see where the database gets modified.
class RsDataBaseTester
{
public:
    RsDataBaseTester(RsGeneralDataService *store,const RsGxsGroupId& grpId,const std::string& info)
            :_grpId(grpId),_store(store),_info(info)
    {
        //std::cerr << "RsDataBaseTester: (" << _info << ") retrieving messages for group " << grpId << std::endl;
        _store->retrieveMsgIds(_grpId, _msgIds1) ;
    }

    ~RsDataBaseTester()
    {
        //std::cerr << "RsDataBaseTester: (" << _info << ") testing messages for group " << _grpId << std::endl;
        _store->retrieveMsgIds(_grpId, _msgIds2) ;

        bool all_idendical = true ;
    std::cerr << std::dec ;

        if(_msgIds1.size() != _msgIds2.size())
            std::cerr << "  " << _info << " (EE) The two arrays are different (size1=" << _msgIds1.size() << ", size2=" << _msgIds2.size() << ") !!" << std::endl;
        else
            for(uint32_t i=0;i<_msgIds1.size();++i)
                if(_msgIds1[i] != _msgIds2[i])
                    std::cerr << "  " << _info << " (EE) The two arrays are different for i=" << i << " !!" << std::endl;
    }
    RsGxsGroupId _grpId ;
    RsGeneralDataService *_store ;
    std::vector<RsGxsMessageId> _msgIds1 ;
    std::vector<RsGxsMessageId> _msgIds2 ;
    std::string _info ;
};
#endif

RsGenExchange::~RsGenExchange()
{
    // need to destruct in a certain order (bad thing, TODO: put down instance ownership rules!)
    delete mNetService;

    delete mDataAccess;
    mDataAccess = NULL;

    delete mDataStore;
    mDataStore = NULL;

}

void RsGenExchange::run()
{

    double timeDelta = 0.1; // slow tick

    while(isRunning())
    {
        tick();

#ifndef WINDOWS_SYS
        usleep((int) (timeDelta * 1000000));
#else
        Sleep((int) (timeDelta * 1000));
#endif
    }
}

void RsGenExchange::tick()
{
	// Meta Changes should happen first.
	// This is important, as services want to change Meta, then get results.
	// Services shouldn't rely on this ordering - but some do.
	processGrpMetaChanges();
	processMsgMetaChanges();

	mDataAccess->processRequests();

	publishGrps();

	publishMsgs();

	processGroupUpdatePublish();

	processGroupDelete();

	processRecvdData();

	if(!mNotifications.empty())
	{
		notifyChanges(mNotifications);
		mNotifications.clear();
	}

	// implemented service tick function
	service_tick();

	time_t now = time(NULL);

	if((mLastClean + MSG_CLEANUP_PERIOD < now) || mCleaning)
	{
		if(mMsgCleanUp)
		{
			if(mMsgCleanUp->clean())
			{
				mCleaning = false;
				delete mMsgCleanUp;
				mMsgCleanUp = NULL;
				mLastClean = time(NULL);
			}

		}else
		{
			mMsgCleanUp = new RsGxsMessageCleanUp(mDataStore, MESSAGE_STORE_PERIOD, 1);
			mCleaning = true;
		}
	}

	now = time(NULL);
	if(mChecking || (mLastCheck + INTEGRITY_CHECK_PERIOD < now))
	{
		if(mIntegrityCheck)
		{
			if(mIntegrityCheck->isDone())
			{
				mIntegrityCheck->join();
				delete mIntegrityCheck;
				mIntegrityCheck = NULL;
				mLastCheck = time(NULL);
				mChecking = false;
			}
		}
		else
		{
			mIntegrityCheck = new RsGxsIntegrityCheck(mDataStore);
			mIntegrityCheck->start();
			mChecking = true;
		}
	}
}

bool RsGenExchange::messagePublicationTest(const RsGxsMsgMetaData& meta)
{
	time_t now = time(NULL) ;

	return meta.mMsgStatus & GXS_SERV::GXS_MSG_STATUS_KEEP || meta.mPublishTs + MESSAGE_STORE_PERIOD >= now ;
}

bool RsGenExchange::acknowledgeTokenMsg(const uint32_t& token,
                RsGxsGrpMsgIdPair& msgId)
{
	RsStackMutex stack(mGenMtx);

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::acknowledgeTokenMsg(). token=" << token << std::endl;
#endif
	std::map<uint32_t, RsGxsGrpMsgIdPair >::iterator mit = mMsgNotify.find(token);

	if(mit == mMsgNotify.end())
	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "  no notification found for this token." << std::endl;
#endif
		return false;
	}


	msgId = mit->second;

	// no dump token as client has ackowledged its completion
	mDataAccess->disposeOfPublicToken(token);

#ifdef GEN_EXCH_DEBUG
	std::cerr << "  found grpId=" << msgId.first <<", msgId=" << msgId.second << std::endl;
	std::cerr << "  disposing token from mDataAccess" << std::endl;
#endif
	return true;
}



bool RsGenExchange::acknowledgeTokenGrp(const uint32_t& token, RsGxsGroupId& grpId)
{
	RsStackMutex stack(mGenMtx);

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::acknowledgeTokenGrp(). token=" << token << std::endl;
#endif
	std::map<uint32_t, RsGxsGroupId >::iterator mit =
                        mGrpNotify.find(token);

	if(mit == mGrpNotify.end())
	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "  no notification found for this token." << std::endl;
#endif
		return false;
	}

	grpId = mit->second;

	// no dump token as client has ackowledged its completion
	mDataAccess->disposeOfPublicToken(token);

#ifdef GEN_EXCH_DEBUG
	std::cerr << "  found grpId=" << grpId << std::endl;
	std::cerr << "  disposing token from mDataAccess" << std::endl;
#endif
	return true;
}

void RsGenExchange::generateGroupKeys(RsTlvSecurityKeySet& privatekeySet, RsTlvSecurityKeySet& publickeySet, bool genPublishKeys)
{
    /* create Keys */
    RsTlvSecurityKey adminKey, privAdminKey;
	 GxsSecurity::generateKeyPair(adminKey,privAdminKey) ;

    // for now all public
    adminKey.keyFlags = RSTLV_KEY_DISTRIB_ADMIN | RSTLV_KEY_TYPE_PUBLIC_ONLY;
    privAdminKey.keyFlags = RSTLV_KEY_DISTRIB_ADMIN | RSTLV_KEY_TYPE_FULL;

    publickeySet.keys[adminKey.keyId] = adminKey;
    privatekeySet.keys[privAdminKey.keyId] = privAdminKey;

    if(genPublishKeys)
    {
        /* set publish keys */
        RsTlvSecurityKey pubKey, privPubKey;
		  GxsSecurity::generateKeyPair(pubKey,privPubKey) ;

        // for now all public
        pubKey.keyFlags = RSTLV_KEY_DISTRIB_PUBLIC | RSTLV_KEY_TYPE_PUBLIC_ONLY;
        privPubKey.keyFlags = RSTLV_KEY_DISTRIB_PRIVATE | RSTLV_KEY_TYPE_FULL;

        publickeySet.keys[pubKey.keyId] = pubKey;
        privatekeySet.keys[privPubKey.keyId] = privPubKey;
    }
}

void RsGenExchange::generatePublicFromPrivateKeys(const RsTlvSecurityKeySet &privatekeySet, RsTlvSecurityKeySet &publickeySet)
{
	// actually just copy settings of one key except mark its key flags public

	publickeySet = RsTlvSecurityKeySet() ;
	RsTlvSecurityKey pubkey ;

	for(std::map<RsGxsId, RsTlvSecurityKey>::const_iterator cit=privatekeySet.keys.begin(); cit != privatekeySet.keys.end(); ++cit)
		if(GxsSecurity::extractPublicKey(cit->second,pubkey))
			publickeySet.keys.insert(std::make_pair(pubkey.keyId, pubkey));
}

uint8_t RsGenExchange::createGroup(RsNxsGrp *grp, RsTlvSecurityKeySet& privateKeySet, RsTlvSecurityKeySet& publicKeySet)
{
#ifdef GEN_EXCH_DEBUG
    std::cerr << "RsGenExchange::createGroup()";
    std::cerr << std::endl;
#endif

    RsGxsGrpMetaData* meta = grp->metaData;

    /* add public admin and publish keys to grp */

    // find private admin key
    RsTlvSecurityKey privAdminKey;
    std::map<RsGxsId, RsTlvSecurityKey>::iterator mit = privateKeySet.keys.begin();

    bool privKeyFound = false; // private admin key
    for(; mit != privateKeySet.keys.end(); mit++)
    {
        RsTlvSecurityKey& key = mit->second;

        if((key.keyFlags & RSTLV_KEY_DISTRIB_ADMIN) && (key.keyFlags & RSTLV_KEY_TYPE_FULL))
        {
            privAdminKey = key;
            privKeyFound = true;
        }
    }

    if(!privKeyFound)
    {
        std::cerr << "RsGenExchange::createGroup() Missing private ADMIN Key";
	std::cerr << std::endl;

    	return false;
    }

    meta->keys = publicKeySet; // only public keys are included to be transported

    // group is self signing
    // for the creation of group signature
    // only public admin and publish keys are present in meta
    uint32_t metaDataLen = meta->serial_size();
    uint32_t allGrpDataLen = metaDataLen + grp->grp.bin_len;
    char* metaData = new char[metaDataLen];
    char* allGrpData = new char[allGrpDataLen]; // msgData + metaData

    meta->serialise(metaData, metaDataLen);

    // copy msg data and meta in allMsgData buffer
    memcpy(allGrpData, grp->grp.bin_data, grp->grp.bin_len);
    memcpy(allGrpData+(grp->grp.bin_len), metaData, metaDataLen);

    RsTlvKeySignature adminSign;
    bool ok = GxsSecurity::getSignature(allGrpData, allGrpDataLen, privAdminKey, adminSign);

    // add admin sign to grpMeta
    meta->signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_ADMIN] = adminSign;

    RsTlvBinaryData grpData(mServType);
	grpData.setBinData(allGrpData, allGrpDataLen);

    uint8_t ret = createGroupSignatures(meta->signSet, grpData, *(grp->metaData));

    // clean up
    delete[] allGrpData;
    delete[] metaData;

    if (!ok)
    {
		std::cerr << "RsGenExchange::createGroup() ERROR !okay (getSignature error)";
		std::cerr << std::endl;
		return CREATE_FAIL;
    }

    if(ret == SIGN_FAIL)
    {
    	return CREATE_FAIL;
    }else if(ret == SIGN_FAIL_TRY_LATER)
    {
    	return CREATE_FAIL_TRY_LATER;
    }else if(ret == SIGN_SUCCESS)
    {
    	return CREATE_SUCCESS;
    }else{
    	return CREATE_FAIL;
    }
}

int RsGenExchange::createGroupSignatures(RsTlvKeySignatureSet& signSet, RsTlvBinaryData& grpData,
    							RsGxsGrpMetaData& grpMeta)
{
	bool needIdentitySign = false;
    int id_ret;

    uint8_t author_flag = GXS_SERV::GRP_OPTION_AUTHEN_AUTHOR_SIGN;

    PrivacyBitPos pos = GRP_OPTION_BITS;

    // Check required permissions, and allow them to sign it - if they want too - as well!
    if ((!grpMeta.mAuthorId.isNull()) || checkAuthenFlag(pos, author_flag))
    {
        needIdentitySign = true;
        std::cerr << "Needs Identity sign! (Service Flags)";
        std::cerr << std::endl;
    }

    if (needIdentitySign)
    {
        if(mGixs)
        {
            bool haveKey = mGixs->havePrivateKey(grpMeta.mAuthorId);

            if(haveKey)
            {
                RsTlvSecurityKey authorKey;
                mGixs->getPrivateKey(grpMeta.mAuthorId, authorKey);
                RsTlvKeySignature sign;

                if(GxsSecurity::getSignature((char*)grpData.bin_data, grpData.bin_len,
                                                authorKey, sign))
                {
                	id_ret = SIGN_SUCCESS;
                }
                else
                {
                	id_ret = SIGN_FAIL;
                }

                signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_IDENTITY] = sign;
            }
            else
            {
            	mGixs->requestPrivateKey(grpMeta.mAuthorId);

                std::cerr << "RsGenExchange::createGroupSignatures(): ";
                std::cerr << " ERROR AUTHOR KEY: " <<  grpMeta.mAuthorId
                		  << " is not Cached / available for Message Signing\n";
                std::cerr << "RsGenExchange::createGroupSignatures():  Requestiong AUTHOR KEY";
                std::cerr << std::endl;

                id_ret = SIGN_FAIL_TRY_LATER;
            }
        }
        else
        {
            std::cerr << "RsGenExchange::createGroupSignatures()";
            std::cerr << "Gixs not enabled while request identity signature validation!" << std::endl;
            id_ret = SIGN_FAIL;
        }
    }
    else
    {
    	id_ret = SIGN_SUCCESS;
    }

	return id_ret;
}

int RsGenExchange::createMsgSignatures(RsTlvKeySignatureSet& signSet, RsTlvBinaryData& msgData,
                                        const RsGxsMsgMetaData& msgMeta, RsGxsGrpMetaData& grpMeta)
{
    bool needPublishSign = false, needIdentitySign = false;
    uint32_t grpFlag = grpMeta.mGroupFlags;

    bool publishSignSuccess = false;

    std::cerr << "RsGenExchange::createMsgSignatures() for Msg.mMsgName: " << msgMeta.mMsgName;
    std::cerr << std::endl;


    // publish signature is determined by whether group is public or not
    // for private group signature is not needed as it needs decrypting with
    // the private publish key anyways

    // restricted is a special case which heeds whether publish sign needs to be checked or not
    // one may or may not want

    uint8_t author_flag = GXS_SERV::MSG_AUTHEN_ROOT_AUTHOR_SIGN;
    uint8_t publish_flag = GXS_SERV::MSG_AUTHEN_ROOT_PUBLISH_SIGN;

    if(!msgMeta.mParentId.isNull())
    {
        // Child Message.
        author_flag = GXS_SERV::MSG_AUTHEN_CHILD_AUTHOR_SIGN;
        publish_flag = GXS_SERV::MSG_AUTHEN_CHILD_PUBLISH_SIGN;
    }

    PrivacyBitPos pos = PUBLIC_GRP_BITS;
    if (grpFlag & GXS_SERV::FLAG_PRIVACY_RESTRICTED)
    {
        pos = RESTRICTED_GRP_BITS;
    }
    else if (grpFlag & GXS_SERV::FLAG_PRIVACY_PRIVATE)
    {
        pos = PRIVATE_GRP_BITS;
    }
    
    needIdentitySign = false;
    needPublishSign = false;
    if (checkAuthenFlag(pos, publish_flag))
    {
        needPublishSign = true;
        std::cerr << "Needs Publish sign! (Service Flags)";
        std::cerr << std::endl;
    }

    // Check required permissions, and allow them to sign it - if they want too - as well!
    if (checkAuthenFlag(pos, author_flag))
    {
        needIdentitySign = true;
        std::cerr << "Needs Identity sign! (Service Flags)";
        std::cerr << std::endl;
    }

    if (!msgMeta.mAuthorId.isNull())
    {
        needIdentitySign = true;
        std::cerr << "Needs Identity sign! (AuthorId Exists)";
        std::cerr << std::endl;
    }

    if(needPublishSign)
    {
        // public and shared is publish key
        RsTlvSecurityKeySet& keys = grpMeta.keys;
        RsTlvSecurityKey* pubKey;

        std::map<RsGxsId, RsTlvSecurityKey>::iterator mit =
                        keys.keys.begin(), mit_end = keys.keys.end();
        bool pub_key_found = false;
        for(; mit != mit_end; mit++)
        {

                pub_key_found = mit->second.keyFlags == (RSTLV_KEY_DISTRIB_PRIVATE | RSTLV_KEY_TYPE_FULL);
                if(pub_key_found)
                        break;
        }

        if (pub_key_found)
        {
            // private publish key
            pubKey = &(mit->second);

            RsTlvKeySignature pubSign = signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_PUBLISH];

            publishSignSuccess = GxsSecurity::getSignature((char*)msgData.bin_data, msgData.bin_len, *pubKey, pubSign);

            //place signature in msg meta
            signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_PUBLISH] = pubSign;
        }else
        {
        	std::cerr << "RsGenExchange::createMsgSignatures()";
			std::cerr << " ERROR Cannot find PUBLISH KEY for Message Signing!";
			std::cerr << " ERROR Publish Sign failed!";
			std::cerr << std::endl;
        }

    }
    else // publish sign not needed so set as successful
    {
    	publishSignSuccess = true;
    }

    int id_ret;

    if (needIdentitySign)
    {
        if(mGixs)
        {
            bool haveKey = mGixs->havePrivateKey(msgMeta.mAuthorId);

            if(haveKey)
            {
                RsTlvSecurityKey authorKey;
                mGixs->getPrivateKey(msgMeta.mAuthorId, authorKey);
                RsTlvKeySignature sign;

                if(GxsSecurity::getSignature((char*)msgData.bin_data, msgData.bin_len,
                                                authorKey, sign))
                {
                	id_ret = SIGN_SUCCESS;
                }
                else
                {
                	id_ret = SIGN_FAIL;
                }

                signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_IDENTITY] = sign;
            }
            else
            {
            	mGixs->requestPrivateKey(msgMeta.mAuthorId);

                std::cerr << "RsGenExchange::createMsgSignatures(): ";
                std::cerr << " ERROR AUTHOR KEY: " <<  msgMeta.mAuthorId
                		  << " is not Cached / available for Message Signing\n";
                std::cerr << "RsGenExchange::createMsgSignatures():  Requestiong AUTHOR KEY";
                std::cerr << std::endl;

                id_ret = SIGN_FAIL_TRY_LATER;
            }
        }
        else
        {
            std::cerr << "RsGenExchange::createMsgSignatures()";
            std::cerr << "Gixs not enabled while request identity signature validation!" << std::endl;
            id_ret = SIGN_FAIL;
        }
    }
    else
    {
    	id_ret = SIGN_SUCCESS;
    }

    if(publishSignSuccess)
    {
    	return id_ret;
    }
    else
    {
    	return SIGN_FAIL;
    }
}

int RsGenExchange::createMessage(RsNxsMsg* msg)
{
	const RsGxsGroupId& id = msg->grpId;

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::createMessage() " << std::endl;
#endif
	std::map<RsGxsGroupId, RsGxsGrpMetaData*> metaMap;

	metaMap.insert(std::make_pair(id, (RsGxsGrpMetaData*)(NULL)));
	mDataStore->retrieveGxsGrpMetaData(metaMap);

	RsGxsMsgMetaData &meta = *(msg->metaData);

	int ret_val;

	if(!metaMap[id])
	{
		return CREATE_FAIL;
	}
	else
	{
		// get publish key
		RsGxsGrpMetaData* grpMeta = metaMap[id];

		uint32_t metaDataLen = meta.serial_size();
		uint32_t allMsgDataLen = metaDataLen + msg->msg.bin_len;
		char* metaData = new char[metaDataLen];
		char* allMsgData = new char[allMsgDataLen]; // msgData + metaData

		meta.serialise(metaData, &metaDataLen);

		// copy msg data and meta in allmsgData buffer
		memcpy(allMsgData, msg->msg.bin_data, msg->msg.bin_len);
		memcpy(allMsgData+(msg->msg.bin_len), metaData, metaDataLen);

		RsTlvBinaryData msgData(0);

		msgData.setBinData(allMsgData, allMsgDataLen);

		// create signatures
	   ret_val = createMsgSignatures(meta.signSet, msgData, meta, *grpMeta);


		// get hash of msg data to create msg id
		pqihash hash;
		hash.addData(allMsgData, allMsgDataLen);
		RsFileHash hashId;
		hash.Complete(hashId);
		msg->msgId = hashId;

		// assign msg id to msg meta
		msg->metaData->mMsgId = msg->msgId;

		delete[] metaData;
		delete[] allMsgData;

		delete grpMeta;
	}

	if(ret_val == SIGN_FAIL)
		return CREATE_FAIL;
	else if(ret_val == SIGN_FAIL_TRY_LATER)
		return CREATE_FAIL_TRY_LATER;
	else if(ret_val == SIGN_SUCCESS)
		return CREATE_SUCCESS;
	else
	{
		std::cerr << "Unknown return value from signature attempt!";
		return CREATE_FAIL;
	}
}

int RsGenExchange::validateMsg(RsNxsMsg *msg, const uint32_t& grpFlag, RsTlvSecurityKeySet& grpKeySet)
{
    bool needIdentitySign = false;
    bool needPublishSign = false;
    bool publishValidate = true, idValidate = true;

    uint8_t author_flag = GXS_SERV::MSG_AUTHEN_ROOT_AUTHOR_SIGN;
    uint8_t publish_flag = GXS_SERV::MSG_AUTHEN_ROOT_PUBLISH_SIGN;

    if(!msg->metaData->mParentId.isNull())
    {
        // Child Message.
        author_flag = GXS_SERV::MSG_AUTHEN_CHILD_AUTHOR_SIGN;
        publish_flag = GXS_SERV::MSG_AUTHEN_CHILD_PUBLISH_SIGN;
    }

    PrivacyBitPos pos = PUBLIC_GRP_BITS;
    if (grpFlag & GXS_SERV::FLAG_PRIVACY_RESTRICTED)
    {
        pos = RESTRICTED_GRP_BITS;
    }
    else if (grpFlag & GXS_SERV::FLAG_PRIVACY_PRIVATE)
    {
        pos = PRIVATE_GRP_BITS;
    }
    
    if (checkAuthenFlag(pos, publish_flag))
        needPublishSign = true;

    // Check required permissions, if they have signed it anyway - we need to validate it.
    if ((checkAuthenFlag(pos, author_flag)) || (!msg->metaData->mAuthorId.isNull()))
        needIdentitySign = true;


    RsGxsMsgMetaData& metaData = *(msg->metaData);

    if(needPublishSign)
    {
        RsTlvKeySignature sign = metaData.signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_PUBLISH];

        std::map<RsGxsId, RsTlvSecurityKey>& keys = grpKeySet.keys;
        std::map<RsGxsId, RsTlvSecurityKey>::iterator mit = keys.begin();

        RsGxsId keyId;
        for(; mit != keys.end() ; mit++)
        {
            RsTlvSecurityKey& key = mit->second;

            if((key.keyFlags & RSTLV_KEY_DISTRIB_PUBLIC) &&
               (key.keyFlags & RSTLV_KEY_TYPE_PUBLIC_ONLY))
            {
                keyId = key.keyId;
            }
        }

        if(!keyId.isNull())
        {
            RsTlvSecurityKey& key = keys[keyId];
            publishValidate &= GxsSecurity::validateNxsMsg(*msg, sign, key);
        }
        else
        {
            publishValidate = false;
        }
    }
    else
    {
    	publishValidate = true;
    }



    if(needIdentitySign)
    {
        if(mGixs)
        {
            bool haveKey = mGixs->haveKey(metaData.mAuthorId);

            if(haveKey)
            {

                RsTlvSecurityKey authorKey;
                bool auth_key_fetched = mGixs->getKey(metaData.mAuthorId, authorKey) == 1;

				if (auth_key_fetched)
				{

	                RsTlvKeySignature sign = metaData.signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_IDENTITY];
	                idValidate &= GxsSecurity::validateNxsMsg(*msg, sign, authorKey);
				}
				else
				{
                     std::cerr << "RsGenExchange::validateMsg()";
                     std::cerr << " ERROR Cannot Retrieve AUTHOR KEY for Message Validation";
                     std::cerr << std::endl;
                     idValidate = false;
                }

            }else
            {
                std::list<RsPeerId> peers;
                peers.push_back(msg->PeerId());
                mGixs->requestKey(metaData.mAuthorId, peers);
                return VALIDATE_FAIL_TRY_LATER;
            }
        }
        else
        {
#ifdef GEN_EXCH_DEBUG
            std::cerr << "Gixs not enabled while request identity signature validation!" << std::endl;
#endif
            idValidate = false;
        }
    }
    else
    {
    	idValidate = true;
    }

    if(publishValidate && idValidate)
    	return VALIDATE_SUCCESS;
    else
    	return VALIDATE_FAIL;

}

int RsGenExchange::validateGrp(RsNxsGrp* grp)
{

	bool needIdentitySign = false, idValidate = false;
	RsGxsGrpMetaData& metaData = *(grp->metaData);

    uint8_t author_flag = GXS_SERV::GRP_OPTION_AUTHEN_AUTHOR_SIGN;

    PrivacyBitPos pos = GRP_OPTION_BITS;

    // Check required permissions, and allow them to sign it - if they want too - as well!
    if (!(metaData.mAuthorId.isNull()) || checkAuthenFlag(pos, author_flag))
    {
        needIdentitySign = true;
        std::cerr << "Needs Identity sign! (Service Flags)";
        std::cerr << std::endl;
    }

	if(needIdentitySign)
	{
		if(mGixs)
		{
			bool haveKey = mGixs->haveKey(metaData.mAuthorId);

			if(haveKey)
			{

				RsTlvSecurityKey authorKey;
				bool auth_key_fetched = mGixs->getKey(metaData.mAuthorId, authorKey) == 1;

				if (auth_key_fetched)
				{

					RsTlvKeySignature sign = metaData.signSet.keySignSet[GXS_SERV::FLAG_AUTHEN_IDENTITY];
					idValidate = GxsSecurity::validateNxsGrp(*grp, sign, authorKey);
				}
				else
				{
					 std::cerr << "RsGenExchange::validateGrp()";
					 std::cerr << " ERROR Cannot Retrieve AUTHOR KEY for Group Sign Validation";
					 std::cerr << std::endl;
					 idValidate = false;
				}

			}else
			{
                std::list<RsPeerId> peers;
				peers.push_back(grp->PeerId());
				mGixs->requestKey(metaData.mAuthorId, peers);
				return VALIDATE_FAIL_TRY_LATER;
			}
		}
		else
		{
#ifdef GEN_EXCH_DEBUG
			std::cerr << "Gixs not enabled while request identity signature validation!" << std::endl;
#endif
			idValidate = false;
		}
	}
	else
	{
		idValidate = true;
	}

	if(idValidate)
		return VALIDATE_SUCCESS;
	else
		return VALIDATE_FAIL;

}

bool RsGenExchange::checkAuthenFlag(const PrivacyBitPos& pos, const uint8_t& flag) const
{
    std::cerr << "RsGenExchange::checkMsgAuthenFlag(pos: " << pos << " flag: ";
    std::cerr << (int) flag << " mAuthenPolicy: " << mAuthenPolicy << ")";
    std::cerr << std::endl;

    switch(pos)
    {
        case PUBLIC_GRP_BITS:
            return mAuthenPolicy & flag;
            break;
        case RESTRICTED_GRP_BITS:
            return flag & (mAuthenPolicy >> RESTR_GRP_OFFSET);
            break;
        case PRIVATE_GRP_BITS:
            return  flag & (mAuthenPolicy >> PRIV_GRP_OFFSET);
            break;
        case GRP_OPTION_BITS:
            return  flag & (mAuthenPolicy >> GRP_OPTIONS_OFFSET);
            break;
        default:
            std::cerr << "pos option not recognised";
            return false;
    }
}

void RsGenExchange::receiveChanges(std::vector<RsGxsNotify*>& changes)
{
	RsStackMutex stack(mGenMtx);

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::receiveChanges()" << std::endl;
#endif
	std::vector<RsGxsNotify*>::iterator vit = changes.begin();

	for(; vit != changes.end(); vit++)
	{
		RsGxsNotify* n = *vit;
		RsGxsGroupChange* gc;
		RsGxsMsgChange* mc;
		if((mc = dynamic_cast<RsGxsMsgChange*>(n)) != NULL)
		{
				mMsgChange.push_back(mc);
		}
		else if((gc = dynamic_cast<RsGxsGroupChange*>(n)) != NULL)
		{
				mGroupChange.push_back(gc);
		}
		else
		{
#warning cyril: very weird code. Why delete an element without removing it from the array??
			delete n;	
		}
	}

}

void RsGenExchange::msgsChanged(std::map<RsGxsGroupId, std::vector<RsGxsMessageId> >& msgs, std::map<RsGxsGroupId, std::vector<RsGxsMessageId> >& msgsMeta)
{
	if(mGenMtx.trylock())
	{
		while(!mMsgChange.empty())
		{
			RsGxsMsgChange* mc = mMsgChange.back();
			if (mc->metaChange())
			{
				msgsMeta = mc->msgChangeMap;
			}
			else
			{
				msgs = mc->msgChangeMap;
			}
			mMsgChange.pop_back();
			delete mc;
		}
            mGenMtx.unlock();
	}
}

void RsGenExchange::groupsChanged(std::list<RsGxsGroupId>& grpIds, std::list<RsGxsGroupId>& grpIdsMeta)
{

	if(mGenMtx.trylock())
	{
		while(!mGroupChange.empty())
		{
			RsGxsGroupChange* gc = mGroupChange.back();
			std::list<RsGxsGroupId>& gList = gc->mGrpIdList;
			std::list<RsGxsGroupId>::iterator lit = gList.begin();
			for(; lit != gList.end(); lit++)
				if (gc->metaChange())
				{
					grpIdsMeta.push_back(*lit);
				}
				else
				{
					grpIds.push_back(*lit);
				}

			mGroupChange.pop_back();
			delete gc;
		}
            mGenMtx.unlock();
	}
}

bool RsGenExchange::subscribeToGroup(uint32_t& token, const RsGxsGroupId& grpId, bool subscribe)
{
    if(subscribe)
        setGroupSubscribeFlags(token, grpId, GXS_SERV::GROUP_SUBSCRIBE_SUBSCRIBED,
        		(GXS_SERV::GROUP_SUBSCRIBE_SUBSCRIBED | GXS_SERV::GROUP_SUBSCRIBE_NOT_SUBSCRIBED));
    else
        setGroupSubscribeFlags(token, grpId, GXS_SERV::GROUP_SUBSCRIBE_NOT_SUBSCRIBED,
        		(GXS_SERV::GROUP_SUBSCRIBE_SUBSCRIBED | GXS_SERV::GROUP_SUBSCRIBE_NOT_SUBSCRIBED));

	return true;
}

bool RsGenExchange::getGroupStatistic(const uint32_t& token, GxsGroupStatistic& stats)
{
    return mDataAccess->getGroupStatistic(token, stats);
}

bool RsGenExchange::getServiceStatistic(const uint32_t& token, GxsServiceStatistic& stats)
{
    return mDataAccess->getServiceStatistic(token, stats);
}

bool RsGenExchange::updated(bool willCallGrpChanged, bool willCallMsgChanged)
{
	bool changed = false;

	if(mGenMtx.trylock())
	{
		changed =  (!mGroupChange.empty() || !mMsgChange.empty());

		if(!willCallGrpChanged)
		{
			while(!mGroupChange.empty())
			{
				RsGxsGroupChange* gc = mGroupChange.back();
				mGroupChange.pop_back();
				delete gc;
			}
		}

		if(!willCallMsgChanged)
		{
			while(!mMsgChange.empty())
			{
				RsGxsMsgChange* mc = mMsgChange.back();
				mMsgChange.pop_back();
				delete mc;
			}
		}

		mGenMtx.unlock();
	}

	return changed;
}

bool RsGenExchange::getGroupList(const uint32_t &token, std::list<RsGxsGroupId> &groupIds)
{
	return mDataAccess->getGroupList(token, groupIds);

}

bool RsGenExchange::getMsgList(const uint32_t &token,
                               GxsMsgIdResult &msgIds)
{
	return mDataAccess->getMsgList(token, msgIds);
}

bool RsGenExchange::getMsgRelatedList(const uint32_t &token, MsgRelatedIdResult &msgIds)
{
    return mDataAccess->getMsgRelatedList(token, msgIds);
}

bool RsGenExchange::getGroupMeta(const uint32_t &token, std::list<RsGroupMetaData> &groupInfo)
{
	std::list<RsGxsGrpMetaData*> metaL;
	bool ok = mDataAccess->getGroupSummary(token, metaL);

	std::list<RsGxsGrpMetaData*>::iterator lit = metaL.begin();
	RsGroupMetaData m;
	for(; lit != metaL.end(); lit++)
	{
		RsGxsGrpMetaData& gMeta = *(*lit);
		m = gMeta;
		groupInfo.push_back(m);
		delete (*lit);
	}

	return ok;
}


bool RsGenExchange::getMsgMeta(const uint32_t &token,
                               GxsMsgMetaMap &msgInfo)
{
#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::getMsgMeta(): retrieving meta data for token " << token << std::endl;
#endif
	std::list<RsGxsMsgMetaData*> metaL;
	GxsMsgMetaResult result;
	bool ok = mDataAccess->getMsgSummary(token, result);

	GxsMsgMetaResult::iterator mit = result.begin();

	for(; mit != result.end(); mit++)
	{
		std::vector<RsGxsMsgMetaData*>& metaV = mit->second;

		msgInfo[mit->first] = std::vector<RsMsgMetaData>();
		std::vector<RsMsgMetaData>& msgInfoV = msgInfo[mit->first];

		std::vector<RsGxsMsgMetaData*>::iterator vit = metaV.begin();
		RsMsgMetaData meta;
		for(; vit != metaV.end(); vit++)
		{
			RsGxsMsgMetaData& m = *(*vit);
			meta = m;
			msgInfoV.push_back(meta);
			delete *vit;
		}
		metaV.clear();
	}

	return ok;
}

bool RsGenExchange::getMsgRelatedMeta(const uint32_t &token, GxsMsgRelatedMetaMap &msgMeta)
{
        MsgRelatedMetaResult result;
        bool ok = mDataAccess->getMsgRelatedSummary(token, result);

        MsgRelatedMetaResult::iterator mit = result.begin();

        for(; mit != result.end(); mit++)
        {
                std::vector<RsGxsMsgMetaData*>& metaV = mit->second;

                msgMeta[mit->first] = std::vector<RsMsgMetaData>();
                std::vector<RsMsgMetaData>& msgInfoV = msgMeta[mit->first];

                std::vector<RsGxsMsgMetaData*>::iterator vit = metaV.begin();
                RsMsgMetaData meta;
                for(; vit != metaV.end(); vit++)
                {
                        RsGxsMsgMetaData& m = *(*vit);
                        meta = m;
                        msgInfoV.push_back(meta);
                        delete *vit;
                }
                metaV.clear();
        }

        return ok;
}


bool RsGenExchange::getGroupData(const uint32_t &token, std::vector<RsGxsGrpItem *>& grpItem)
{
	std::list<RsNxsGrp*> nxsGrps;
	bool ok = mDataAccess->getGroupData(token, nxsGrps);

	std::list<RsNxsGrp*>::iterator lit = nxsGrps.begin();

	std::cerr << "RsGenExchange::getGroupData() RsNxsGrp::len: " << nxsGrps.size();
	std::cerr << std::endl;

	if(ok)
	{
		for(; lit != nxsGrps.end(); lit++)
		{
			RsTlvBinaryData& data = (*lit)->grp;
			RsItem* item = NULL;

			if(data.bin_len != 0)
				item = mSerialiser->deserialise(data.bin_data, &data.bin_len);

			if(item)
			{
				RsGxsGrpItem* gItem = dynamic_cast<RsGxsGrpItem*>(item);
				if (gItem)
				{
					gItem->meta = *((*lit)->metaData);
					grpItem.push_back(gItem);
				}
				else
				{
					std::cerr << "RsGenExchange::getGroupData() deserialisation/dynamic_cast ERROR";
					std::cerr << std::endl;
					delete item;
				}
			}
			else
			{
				std::cerr << "RsGenExchange::getGroupData() ERROR deserialising item";
				std::cerr << std::endl;
			}
			delete *lit;
		}
	}
	return ok;
}

bool RsGenExchange::getMsgData(const uint32_t &token, GxsMsgDataMap &msgItems)
{
	RsStackMutex stack(mGenMtx);
	NxsMsgDataResult msgResult;
	bool ok = mDataAccess->getMsgData(token, msgResult);
	NxsMsgDataResult::iterator mit = msgResult.begin();

	if(ok)
	{
		for(; mit != msgResult.end(); mit++)
		{
			std::vector<RsGxsMsgItem*> gxsMsgItems;
			const RsGxsGroupId& grpId = mit->first;
			std::vector<RsNxsMsg*>& nxsMsgsV = mit->second;
			std::vector<RsNxsMsg*>::iterator vit = nxsMsgsV.begin();
			for(; vit != nxsMsgsV.end(); vit++)
			{
				RsNxsMsg*& msg = *vit;
				RsItem* item = NULL;

				if(msg->msg.bin_len != 0)
					item = mSerialiser->deserialise(msg->msg.bin_data, &msg->msg.bin_len);

				if (item)
				{
					RsGxsMsgItem* mItem = dynamic_cast<RsGxsMsgItem*>(item);
					if (mItem)
					{
						mItem->meta = *((*vit)->metaData); // get meta info from nxs msg
						gxsMsgItems.push_back(mItem);
					}
					else
					{
						std::cerr << "RsGenExchange::getMsgData() deserialisation/dynamic_cast ERROR";
						std::cerr << std::endl;
						delete item;
					}
				}
				else
				{
					std::cerr << "RsGenExchange::getMsgData() deserialisation ERROR";
					std::cerr << std::endl;
				}
				delete msg;
			}
			msgItems[grpId] = gxsMsgItems;
		}
	}
	return ok;
}

bool RsGenExchange::getMsgRelatedData(const uint32_t &token, GxsMsgRelatedDataMap &msgItems)
{
    RsStackMutex stack(mGenMtx);
    NxsMsgRelatedDataResult msgResult;
    bool ok = mDataAccess->getMsgRelatedData(token, msgResult);


    if(ok)
    {
    	NxsMsgRelatedDataResult::iterator mit = msgResult.begin();
        for(; mit != msgResult.end(); mit++)
        {
            std::vector<RsGxsMsgItem*> gxsMsgItems;
            const RsGxsGrpMsgIdPair& msgId = mit->first;
            std::vector<RsNxsMsg*>& nxsMsgsV = mit->second;
            std::vector<RsNxsMsg*>::iterator vit
            = nxsMsgsV.begin();
            for(; vit != nxsMsgsV.end(); vit++)
            {
                RsNxsMsg*& msg = *vit;
                RsItem* item = NULL;

                if(msg->msg.bin_len != 0)
                	item = mSerialiser->deserialise(msg->msg.bin_data,
                                &msg->msg.bin_len);
				if (item)
				{
					RsGxsMsgItem* mItem = dynamic_cast<RsGxsMsgItem*>(item);

					if (mItem)
					{
							mItem->meta = *((*vit)->metaData); // get meta info from nxs msg
							gxsMsgItems.push_back(mItem);
					}
					else
					{
						std::cerr << "RsGenExchange::getMsgRelatedData() deserialisation/dynamic_cast ERROR";
						std::cerr << std::endl;
						delete item;
					}
				}
				else
				{
					std::cerr << "RsGenExchange::getMsgRelatedData() deserialisation ERROR";
					std::cerr << std::endl;
				}

                delete msg;
            }
            msgItems[msgId] = gxsMsgItems;
        }
    }
    return ok;
}




RsTokenService* RsGenExchange::getTokenService()
{
    return mDataAccess;
}


bool RsGenExchange::setAuthenPolicyFlag(const uint8_t &msgFlag, uint32_t& authenFlag, const PrivacyBitPos &pos)
{
    uint32_t temp = 0;
    temp = msgFlag;

    switch(pos)
    {
        case PUBLIC_GRP_BITS:
            authenFlag &= ~PUB_GRP_MASK;
            authenFlag |= temp;
            break;
        case RESTRICTED_GRP_BITS:
            authenFlag &= ~RESTR_GRP_MASK;
            authenFlag |= (temp << RESTR_GRP_OFFSET);
            break;
        case PRIVATE_GRP_BITS:
            authenFlag &= ~PRIV_GRP_MASK;
            authenFlag |= (temp << PRIV_GRP_OFFSET);
            break;
        case GRP_OPTION_BITS:
            authenFlag &= ~GRP_OPTIONS_MASK;
            authenFlag |= (temp << GRP_OPTIONS_OFFSET);
            break;
        default:
            std::cerr << "pos option not recognised";
            return false;
    }
    return true;
}

void RsGenExchange::notifyNewGroups(std::vector<RsNxsGrp *> &groups)
{
    RsStackMutex stack(mGenMtx);

    std::vector<RsNxsGrp*>::iterator vit = groups.begin();

    // store these for tick() to pick them up
    for(; vit != groups.end(); vit++)
    {
    	RsNxsGrp* grp = *vit;
    	NxsGrpPendValidVect::iterator received = std::find(mReceivedGrps.begin(),
    			mReceivedGrps.end(), grp->grpId);

    	// drop group if you already have them
    	// TODO: move this to nxs layer to save bandwidth
    	if(received == mReceivedGrps.end())
    	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "RsGenExchange::notifyNewGroups() Received GrpId: " << grp->grpId;
		std::cerr << std::endl;
#endif

    		GxsPendingItem<RsNxsGrp*, RsGxsGroupId> gpsi(grp, grp->grpId);
    		mReceivedGrps.push_back(gpsi);
    	}
    	else
    	{
    		delete grp;
    	}
    }

}

void RsGenExchange::notifyNewMessages(std::vector<RsNxsMsg *>& messages)
{
    RsStackMutex stack(mGenMtx);

    std::vector<RsNxsMsg*>::iterator vit = messages.begin();

    // store these for tick() to pick them up
    for(; vit != messages.end(); vit++)
    {
    	RsNxsMsg* msg = *vit;

    	NxsMsgPendingVect::iterator it =
    			std::find(mMsgPendingValidate.begin(), mMsgPendingValidate.end(), getMsgIdPair(*msg));

    	// if we have msg already just delete it
    	if(it == mMsgPendingValidate.end())
	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "RsGenExchange::notifyNewMessages() Received Msg: ";
		std::cerr << " GrpId: " << msg->grpId;
		std::cerr << " MsgId: " << msg->msgId;
		std::cerr << std::endl;
#endif

    		mReceivedMsgs.push_back(msg);
	}
    	else
	{
    		delete msg;
	}
    }

}


void RsGenExchange::publishGroup(uint32_t& token, RsGxsGrpItem *grpItem)
{

    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();
    GxsGrpPendingSign ggps(grpItem, token);
    mGrpsToPublish.push_back(ggps);

#ifdef GEN_EXCH_DEBUG	
    std::cerr << "RsGenExchange::publishGroup() token: " << token;
    std::cerr << std::endl;
#endif

}


void RsGenExchange::updateGroup(uint32_t& token, RsGxsGrpItem* grpItem)
{
	RsStackMutex stack(mGenMtx);
	token = mDataAccess->generatePublicToken();
        mGroupUpdatePublish.push_back(GroupUpdatePublish(grpItem, token));

#ifdef GEN_EXCH_DEBUG
    std::cerr << "RsGenExchange::updateGroup() token: " << token;
    std::cerr << std::endl;
#endif
}

void RsGenExchange::deleteGroup(uint32_t& token, RsGxsGrpItem* grpItem)
{
	RsStackMutex stack(mGenMtx);
	token = mDataAccess->generatePublicToken();
	mGroupDeletePublish.push_back(GroupDeletePublish(grpItem, token));

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::deleteGroup() token: " << token;
	std::cerr << std::endl;
#endif
}

void RsGenExchange::publishMsg(uint32_t& token, RsGxsMsgItem *msgItem)
{
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();
    mMsgsToPublish.insert(std::make_pair(token, msgItem));

#ifdef GEN_EXCH_DEBUG	
    std::cerr << "RsGenExchange::publishMsg() token: " << token;
    std::cerr << std::endl;
#endif

}

void RsGenExchange::setGroupSubscribeFlags(uint32_t& token, const RsGxsGroupId& grpId, const uint32_t& flag, const uint32_t& mask)
{
	/* TODO APPLY MASK TO FLAGS */
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();

    GrpLocMetaData g;
    g.grpId = grpId;
    g.val.put(RsGeneralDataService::GRP_META_SUBSCRIBE_FLAG, (int32_t)flag);
    g.val.put(RsGeneralDataService::GRP_META_SUBSCRIBE_FLAG+GXS_MASK, (int32_t)mask); // HACK, need to perform mask operation in a non-blocking location
    mGrpLocMetaMap.insert(std::make_pair(token, g));
}

void RsGenExchange::setGroupStatusFlags(uint32_t& token, const RsGxsGroupId& grpId, const uint32_t& status, const uint32_t& mask)
{
	/* TODO APPLY MASK TO FLAGS */
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();

    GrpLocMetaData g;
    g.grpId = grpId;
    g.val.put(RsGeneralDataService::GRP_META_STATUS, (int32_t)status);
    g.val.put(RsGeneralDataService::GRP_META_STATUS+GXS_MASK, (int32_t)mask); // HACK, need to perform mask operation in a non-blocking location
    mGrpLocMetaMap.insert(std::make_pair(token, g));
}


void RsGenExchange::setGroupServiceString(uint32_t& token, const RsGxsGroupId& grpId, const std::string& servString)
{
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();

    GrpLocMetaData g;
    g.grpId = grpId;
    g.val.put(RsGeneralDataService::GRP_META_SERV_STRING, servString);
    mGrpLocMetaMap.insert(std::make_pair(token, g));
}

void RsGenExchange::setMsgStatusFlags(uint32_t& token, const RsGxsGrpMsgIdPair& msgId, const uint32_t& status, const uint32_t& mask)
{
	/* TODO APPLY MASK TO FLAGS */
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();

    MsgLocMetaData m;
    m.val.put(RsGeneralDataService::MSG_META_STATUS, (int32_t)status);
    m.val.put(RsGeneralDataService::MSG_META_STATUS+GXS_MASK, (int32_t)mask); // HACK, need to perform mask operation in a non-blocking location
    m.msgId = msgId;
    mMsgLocMetaMap.insert(std::make_pair(token, m));
}

void RsGenExchange::setMsgServiceString(uint32_t& token, const RsGxsGrpMsgIdPair& msgId, const std::string& servString )
{
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();

    MsgLocMetaData m;
    m.val.put(RsGeneralDataService::MSG_META_SERV_STRING, servString);
    m.msgId = msgId;
    mMsgLocMetaMap.insert(std::make_pair(token, m));
}

void RsGenExchange::processMsgMetaChanges()
{
    RsStackMutex stack(mGenMtx);

    GxsMsgReq msgIds;

    std::map<uint32_t, MsgLocMetaData>::iterator mit = mMsgLocMetaMap.begin(),
    mit_end = mMsgLocMetaMap.end();

    for(; mit != mit_end; mit++)
    {
        MsgLocMetaData& m = mit->second;

        int32_t value, mask;
        bool ok = true;
        bool changed = false;

        // for meta flag changes get flag to apply mask
        if(m.val.getAsInt32(RsGeneralDataService::MSG_META_STATUS, value))
        {
            ok = false;
            if(m.val.getAsInt32(RsGeneralDataService::MSG_META_STATUS+GXS_MASK, mask))
            {
                GxsMsgReq req;
                std::vector<RsGxsMessageId> msgIdV;
                msgIdV.push_back(m.msgId.second);
                req.insert(std::make_pair(m.msgId.first, msgIdV));
                GxsMsgMetaResult result;
                mDataStore->retrieveGxsMsgMetaData(req, result);
                GxsMsgMetaResult::iterator mit = result.find(m.msgId.first);

                if(mit != result.end())
                {
                    std::vector<RsGxsMsgMetaData*>& msgMetaV = mit->second;

                    if(!msgMetaV.empty())
                    {
                        RsGxsMsgMetaData* meta = *(msgMetaV.begin());
                        value = (meta->mMsgStatus & ~mask) | (mask & value);
                        changed = (meta->mMsgStatus != value);
                        m.val.put(RsGeneralDataService::MSG_META_STATUS, value);
                        delete meta;
                        ok = true;
                    }
                }
                m.val.removeKeyValue(RsGeneralDataService::MSG_META_STATUS+GXS_MASK);
            }
        }

        ok &= mDataStore->updateMessageMetaData(m) == 1;
        uint32_t token = mit->first;

        if(ok)
        {
            mDataAccess->updatePublicRequestStatus(token, RsTokenService::GXS_REQUEST_V2_STATUS_COMPLETE);
            if (changed)
            {
                msgIds[m.msgId.first].push_back(m.msgId.second);
            }
        }
        else
        {
            mDataAccess->updatePublicRequestStatus(token, RsTokenService::GXS_REQUEST_V2_STATUS_FAILED);
        }
        mMsgNotify.insert(std::make_pair(token, m.msgId));
    }

    if (!msgIds.empty()) {
        RsGxsMsgChange* c = new RsGxsMsgChange(RsGxsNotify::TYPE_PROCESSED, true);
        c->msgChangeMap = msgIds;
        mNotifications.push_back(c);
    }

    mMsgLocMetaMap.clear();
}

void RsGenExchange::processGrpMetaChanges()
{
    RsStackMutex stack(mGenMtx);

    std::list<RsGxsGroupId> grpChanged;

    std::map<uint32_t, GrpLocMetaData>::iterator mit = mGrpLocMetaMap.begin(),
    mit_end = mGrpLocMetaMap.end();

    for(; mit != mit_end; mit++)
    {
        GrpLocMetaData& g = mit->second;
        uint32_t token = mit->first;

        // process mask
        bool ok = processGrpMask(g.grpId, g.val);

        ok &= mDataStore->updateGroupMetaData(g) == 1;

        if(ok)
        {
            mDataAccess->updatePublicRequestStatus(token, RsTokenService::GXS_REQUEST_V2_STATUS_COMPLETE);
            grpChanged.push_back(g.grpId);
        }else
        {
            mDataAccess->updatePublicRequestStatus(token, RsTokenService::GXS_REQUEST_V2_STATUS_FAILED);
        }
        mGrpNotify.insert(std::make_pair(token, g.grpId));
    }

    if(!grpChanged.empty())
    {
        RsGxsGroupChange* gc = new RsGxsGroupChange(RsGxsNotify::TYPE_PROCESSED, true);
        gc->mGrpIdList = grpChanged;
        mNotifications.push_back(gc);
    }

    mGrpLocMetaMap.clear();
}

bool RsGenExchange::processGrpMask(const RsGxsGroupId& grpId, ContentValue &grpCv)
{
    // first find out which mask is involved
    int32_t value, mask, currValue;
    std::string key;
    RsGxsGrpMetaData* grpMeta = NULL;
    bool ok = false;

    std::map<RsGxsGroupId, RsGxsGrpMetaData* > grpMetaMap;
    std::map<RsGxsGroupId, RsGxsGrpMetaData* >::iterator mit;
    grpMetaMap.insert(std::make_pair(grpId, (RsGxsGrpMetaData*)(NULL)));

    mDataStore->retrieveGxsGrpMetaData(grpMetaMap);

    if((mit = grpMetaMap.find(grpId)) != grpMetaMap.end())
    {
        grpMeta = mit->second;
        ok = true;
    }

    if(grpCv.getAsInt32(RsGeneralDataService::GRP_META_STATUS, value) && grpMeta)
    {
        key = RsGeneralDataService::GRP_META_STATUS;
        currValue = grpMeta->mGroupStatus;
    }
    else if(grpCv.getAsInt32(RsGeneralDataService::GRP_META_SUBSCRIBE_FLAG, value) && grpMeta)
    {
        key = RsGeneralDataService::GRP_META_SUBSCRIBE_FLAG;
        currValue = grpMeta->mSubscribeFlags;
    }else
    {
        if(grpMeta)
            delete grpMeta;
        return !(grpCv.empty());
    }

    ok &= grpCv.getAsInt32(key+GXS_MASK, mask);

    // remove mask entry so it doesn't affect actual entry
    grpCv.removeKeyValue(key+GXS_MASK);

    // apply mask to current value
    value = (currValue & ~mask) | (value & mask);

    grpCv.put(key, value);

    if(grpMeta)
        delete grpMeta;

    return ok;
}

void RsGenExchange::publishMsgs()
{

	RsStackMutex stack(mGenMtx);

	// stick back msgs pending signature
	typedef std::map<uint32_t, GxsPendingItem<RsGxsMsgItem*, uint32_t> > PendSignMap;

	PendSignMap::iterator sign_it = mMsgPendingSign.begin();

	for(; sign_it != mMsgPendingSign.end(); sign_it++)
	{
		GxsPendingItem<RsGxsMsgItem*, uint32_t>& item = sign_it->second;
		mMsgsToPublish.insert(std::make_pair(sign_it->first, item.mItem));
	}

	std::map<RsGxsGroupId, std::vector<RsGxsMessageId> > msgChangeMap;
	std::map<uint32_t, RsGxsMsgItem*>::iterator mit = mMsgsToPublish.begin();

	for(; mit != mMsgsToPublish.end(); mit++)
	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "RsGenExchange::publishMsgs() Publishing a Message";
		std::cerr << std::endl;
#endif

		RsNxsMsg* msg = new RsNxsMsg(mServType);
		RsGxsMsgItem* msgItem = mit->second;
		const uint32_t& token = mit->first;

		msg->grpId = msgItem->meta.mGroupId;

		uint32_t size = mSerialiser->size(msgItem);
		char* mData = new char[size];

		bool serialOk = false;

		// for fatal sign creation
		bool createOk = false;

		// if sign requests to try later
		bool tryLater = false;

		serialOk = mSerialiser->serialise(msgItem, mData, &size);

		if(serialOk)
		{
			msg->msg.setBinData(mData, size);

			// now create meta
			msg->metaData = new RsGxsMsgMetaData();
			*(msg->metaData) = msgItem->meta;

			// assign time stamp
			msg->metaData->mPublishTs = time(NULL);

			// now intialise msg (sign it)
			uint8_t createReturn = createMessage(msg);

			if(createReturn == CREATE_FAIL)
			{
				createOk = false;
			}
			else if(createReturn == CREATE_FAIL_TRY_LATER)
			{
				PendSignMap::iterator pit = mMsgPendingSign.find(token);
				tryLater = true;

				// add to queue of messages waiting for a successful
				// sign attempt
				if(pit == mMsgPendingSign.end())
				{
					GxsPendingItem<RsGxsMsgItem*, uint32_t> gsi(msgItem, token);
					mMsgPendingSign.insert(std::make_pair(token, gsi));
				}
				else
				{
					// remove from attempts queue if over sign
					// attempts limit
					if(pit->second.mAttempts == SIGN_MAX_ATTEMPTS)
					{
						mMsgPendingSign.erase(token);
						tryLater = false;
					}
					else
					{
						pit->second.mAttempts++;
					}
				}

				createOk = false;
			}
			else if(createReturn == CREATE_SUCCESS)
			{
				createOk = true;

				// erase from queue if it exists
				mMsgPendingSign.erase(token);
			}
			else // unknown return, just fail
				createOk = false;



			RsGxsMessageId msgId;
			RsGxsGroupId grpId = msgItem->meta.mGroupId;

			bool validSize = false;

			// check message not over single msg storage limit
			if(createOk)
			{
				validSize = mDataStore->validSize(msg);
			}

			if(createOk && validSize)
			{
				// empty orig msg id means this is the original
				// msg
				if(msg->metaData->mOrigMsgId.isNull())
				{
					msg->metaData->mOrigMsgId = msg->metaData->mMsgId;
				}

				// now serialise meta data
				size = msg->metaData->serial_size();

				char* metaDataBuff = new char[size];
				bool s = msg->metaData->serialise(metaDataBuff, &size);
				s &= msg->meta.setBinData(metaDataBuff, size);

				msg->metaData->mMsgStatus = GXS_SERV::GXS_MSG_STATUS_UNPROCESSED | GXS_SERV::GXS_MSG_STATUS_UNREAD;
				msgId = msg->msgId;
				grpId = msg->grpId;
				msg->metaData->recvTS = time(NULL);
				computeHash(msg->msg, msg->metaData->mHash);
				mDataAccess->addMsgData(msg);
				msgChangeMap[grpId].push_back(msgId);

				delete[] metaDataBuff;

				// add to published to allow acknowledgement
				mMsgNotify.insert(std::make_pair(mit->first, std::make_pair(grpId, msgId)));
				mDataAccess->updatePublicRequestStatus(mit->first, RsTokenService::GXS_REQUEST_V2_STATUS_COMPLETE);

			}
			else
			{
				// delete msg if create msg not ok
				delete msg;

				if(!tryLater)
					mDataAccess->updatePublicRequestStatus(mit->first,
							RsTokenService::GXS_REQUEST_V2_STATUS_FAILED);

				std::cerr << "RsGenExchange::publishMsgs() failed to publish msg " << std::endl;
			}
		}
		else
		{
			std::cerr << "RsGenExchange::publishMsgs() failed to serialise msg " << std::endl;
		}

		delete[] mData;

		if(!tryLater)
			delete msgItem;
	}

	// clear msg item map as we're done publishing them and all
	// entries are invalid
	mMsgsToPublish.clear();

	if(!msgChangeMap.empty())
	{
		RsGxsMsgChange* ch = new RsGxsMsgChange(RsGxsNotify::TYPE_PUBLISH, false);
		ch->msgChangeMap = msgChangeMap;
		mNotifications.push_back(ch);
	}

}

RsGenExchange::ServiceCreate_Return RsGenExchange::service_CreateGroup(RsGxsGrpItem* /* grpItem */,
		RsTlvSecurityKeySet& /* keySet */)
{
#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::service_CreateGroup(): Does nothing"
			  << std::endl;
#endif
	return SERVICE_CREATE_SUCCESS;
}


#define PENDING_SIGN_TIMEOUT 10 //  5 seconds


void RsGenExchange::processGroupUpdatePublish()
{
	RsStackMutex stack(mGenMtx);

	// get keys for group update publish

	// first build meta request map for groups to be updated
	std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMeta;
	std::vector<GroupUpdatePublish>::iterator vit = mGroupUpdatePublish.begin();

	for(; vit != mGroupUpdatePublish.end(); vit++)
	{
		GroupUpdatePublish& gup = *vit;
		const RsGxsGroupId& groupId = gup.grpItem->meta.mGroupId;
		grpMeta.insert(std::make_pair(groupId, (RsGxsGrpMetaData*)(NULL)));
	}

        if(grpMeta.empty())
            return;

	mDataStore->retrieveGxsGrpMetaData(grpMeta);

	// now
	vit = mGroupUpdatePublish.begin();
	for(; vit != mGroupUpdatePublish.end(); vit++)
	{
		GroupUpdatePublish& gup = *vit;
		const RsGxsGroupId& groupId = gup.grpItem->meta.mGroupId;
		std::map<RsGxsGroupId, RsGxsGrpMetaData*>::iterator mit = grpMeta.find(groupId);

		RsGxsGrpMetaData* meta = NULL;
		if(mit == grpMeta.end() || mit->second == NULL)
		{
			std::cerr << "Error! could not find meta of old group to update!" << std::endl;
			mDataAccess->updatePublicRequestStatus(gup.mToken, RsTokenService::GXS_REQUEST_V2_STATUS_FAILED);
			delete gup.grpItem;
			continue;
		}else
		{
			meta = mit->second;
		}


		//gup.grpItem->meta = *meta;
		GxsGrpPendingSign ggps(gup.grpItem, ggps.mToken);

		bool publishAndAdminPrivatePresent = checkKeys(meta->keys);

		if(publishAndAdminPrivatePresent)
		{
			ggps.mPrivateKeys = meta->keys;
			generatePublicFromPrivateKeys(ggps.mPrivateKeys, ggps.mPublicKeys);
			ggps.mHaveKeys = true;
			ggps.mStartTS = time(NULL);
			ggps.mLastAttemptTS = 0;
			ggps.mIsUpdate = true;
			ggps.mToken = gup.mToken;
			mGrpsToPublish.push_back(ggps);
		}else
		{
			delete gup.grpItem;
			mDataAccess->updatePublicRequestStatus(gup.mToken, RsTokenService::GXS_REQUEST_V2_STATUS_FAILED);
		}
		delete meta;
	}

	mGroupUpdatePublish.clear();
}

void RsGenExchange::processGroupDelete()
{
	RsStackMutex stack(mGenMtx);

	// get keys for group delete publish
	typedef std::pair<bool, RsGxsGroupId> GrpNote;
	std::map<uint32_t, GrpNote> toNotify;

	std::vector<GroupDeletePublish>::iterator vit = mGroupDeletePublish.begin();
	for(; vit != mGroupDeletePublish.end(); vit++)
	{
		GroupDeletePublish& gdp = *vit;
		uint32_t token = gdp.mToken;
		const RsGxsGroupId& groupId = gdp.grpItem->meta.mGroupId;
		std::vector<RsGxsGroupId> gprIds;
		gprIds.push_back(groupId);
		mDataStore->removeGroups(gprIds);
		toNotify.insert(std::make_pair(
		                  token, GrpNote(true, RsGxsGroupId())));
	}


	std::list<RsGxsGroupId> grpDeleted;
	std::map<uint32_t, GrpNote>::iterator mit = toNotify.begin();
	for(; mit != toNotify.end(); mit++)
	{
		GrpNote& note = mit->second;
		uint8_t status = note.first ? RsTokenService::GXS_REQUEST_V2_STATUS_COMPLETE
		                            : RsTokenService::GXS_REQUEST_V2_STATUS_FAILED;

		mGrpNotify.insert(std::make_pair(mit->first, note.second));
		mDataAccess->updatePublicRequestStatus(mit->first, status);

		if(note.first)
			grpDeleted.push_back(note.second);
	}

	if(!grpDeleted.empty())
	{
		RsGxsGroupChange* gc = new RsGxsGroupChange(RsGxsNotify::TYPE_PUBLISH, false);
		gc->mGrpIdList = grpDeleted;
		mNotifications.push_back(gc);
	}

	mGroupDeletePublish.clear();
}

bool RsGenExchange::checkKeys(const RsTlvSecurityKeySet& keySet)
{

	typedef std::map<RsGxsId, RsTlvSecurityKey> keyMap;
	const keyMap& allKeys = keySet.keys;
	keyMap::const_iterator cit = allKeys.begin();
        bool adminFound = false, publishFound = false;
	for(; cit != allKeys.end(); cit++)
	{
                const RsTlvSecurityKey& key = cit->second;
                if(key.keyFlags & RSTLV_KEY_TYPE_FULL)
                {
                    if(key.keyFlags & RSTLV_KEY_DISTRIB_ADMIN)
                        adminFound = true;

                    if(key.keyFlags & RSTLV_KEY_DISTRIB_PRIVATE)
                        publishFound = true;

                }
	}

	// user must have both private and public parts of publish and admin keys
        return adminFound && publishFound;
}

void RsGenExchange::publishGrps()
{
    RsStackMutex stack(mGenMtx);
    NxsGrpSignPendVect::iterator vit = mGrpsToPublish.begin();

    typedef std::pair<bool, RsGxsGroupId> GrpNote;
    std::map<uint32_t, GrpNote> toNotify;

    while( vit != mGrpsToPublish.end() )
    {
    	GxsGrpPendingSign& ggps = *vit;

    	/* do intial checks to see if this entry has expired */
    	time_t now = time(NULL) ;
    	uint32_t token = ggps.mToken;


    	if(now > (ggps.mStartTS + PENDING_SIGN_TIMEOUT) )
    	{
    		// timed out
    		toNotify.insert(std::make_pair(
    				token, GrpNote(false, RsGxsGroupId())));
    		delete ggps.mItem;
    		vit = mGrpsToPublish.erase(vit);

    		continue;
    	}

    	RsGxsGroupId grpId;
        RsNxsGrp* grp = new RsNxsGrp(mServType);
        RsGxsGrpItem* grpItem = ggps.mItem;

        RsTlvSecurityKeySet privatekeySet, publicKeySet;

        if(!(ggps.mHaveKeys))
        {
        	generateGroupKeys(privatekeySet, publicKeySet, true);
        	ggps.mHaveKeys = true;
        	ggps.mPrivateKeys = privatekeySet;
        	ggps.mPublicKeys = publicKeySet;
        }
        else
        {
        	privatekeySet = ggps.mPrivateKeys;
        	publicKeySet = ggps.mPublicKeys;
        }

		// find private admin key
        RsTlvSecurityKey privAdminKey;
        std::map<RsGxsId, RsTlvSecurityKey>::iterator mit_keys = privatekeySet.keys.begin();

        bool privKeyFound = false;
        for(; mit_keys != privatekeySet.keys.end(); mit_keys++)
        {
            RsTlvSecurityKey& key = mit_keys->second;

            if(key.keyFlags == (RSTLV_KEY_DISTRIB_ADMIN | RSTLV_KEY_TYPE_FULL))
            {
                privAdminKey = key;
                privKeyFound = true;
            }
        }

        uint8_t create = CREATE_FAIL;

        if(privKeyFound)
        {
		    // get group id from private admin key id
			grpItem->meta.mGroupId = grp->grpId = RsGxsGroupId(privAdminKey.keyId);

			ServiceCreate_Return ret = service_CreateGroup(grpItem, privatekeySet);


			bool serialOk = false, servCreateOk;

			if(ret == SERVICE_CREATE_SUCCESS)
			{
				uint32_t size = mSerialiser->size(grpItem);
				char *gData = new char[size];
				serialOk = mSerialiser->serialise(grpItem, gData, &size);
				grp->grp.setBinData(gData, size);
				delete[] gData;
				servCreateOk = true;

			}else
			{
				servCreateOk = false;
			}

			if(serialOk && servCreateOk)
			{
				grp->metaData = new RsGxsGrpMetaData();
				grpItem->meta.mPublishTs = time(NULL);
                //grpItem->meta.mParentGrpId = std::string("empty");
				*(grp->metaData) = grpItem->meta;

				// TODO: change when publish key optimisation added (public groups don't have publish key
				grp->metaData->mSubscribeFlags = GXS_SERV::GROUP_SUBSCRIBE_ADMIN | GXS_SERV::GROUP_SUBSCRIBE_SUBSCRIBED
						| GXS_SERV::GROUP_SUBSCRIBE_PUBLISH;

				create = createGroup(grp, privatekeySet, publicKeySet);

#ifdef GEN_EXCH_DEBUG
				std::cerr << "RsGenExchange::publishGrps() ";
				std::cerr << " GrpId: " << grp->grpId;
				std::cerr << " CircleType: " << (uint32_t) grp->metaData->mCircleType;
				std::cerr << " CircleId: " << grp->metaData->mCircleId.toStdString();
				std::cerr << std::endl;
#endif

				if(create == CREATE_SUCCESS)
				{

					uint32_t mdSize = grp->metaData->serial_size();
					char* metaData = new char[mdSize];
					serialOk = grp->metaData->serialise(metaData, mdSize);
					grp->meta.setBinData(metaData, mdSize);
					delete[] metaData;

					// place back private keys for publisher
					grp->metaData->keys = privatekeySet;

					if(mDataStore->validSize(grp) && serialOk)
					{
						grpId = grp->grpId;
						computeHash(grp->grp, grp->metaData->mHash);
						grp->metaData->mRecvTS = time(NULL);

						if(ggps.mIsUpdate)
							mDataAccess->updateGroupData(grp);
						else
							mDataAccess->addGroupData(grp);
					}
					else
					{
						create = CREATE_FAIL;
					}
				}
			}
			else if(ret == SERVICE_CREATE_FAIL_TRY_LATER)
			{
				create = CREATE_FAIL_TRY_LATER;
			}
        }
        else
        {
#ifdef GEN_EXCH_DEBUG
        	std::cerr << "RsGenExchange::publishGrps() Could not find private publish keys " << std::endl;
#endif
        	create = CREATE_FAIL;
        }

        if(create == CREATE_FAIL)
        {
#ifdef GEN_EXCH_DEBUG
        	std::cerr << "RsGenExchange::publishGrps() failed to publish grp " << std::endl;
#endif
            delete grp;
            delete grpItem;
            vit = mGrpsToPublish.erase(vit);
            toNotify.insert(std::make_pair(
            		token, GrpNote(false, grpId)));

        }
        else if(create == CREATE_FAIL_TRY_LATER)
        {
#ifdef GEN_EXCH_DEBUG
        	std::cerr << "RsGenExchange::publishGrps() failed grp, trying again " << std::endl;
#endif
        	ggps.mLastAttemptTS = time(NULL);
        	vit++;
        }
        else if(create == CREATE_SUCCESS)
        {
        	delete grpItem;
        	vit = mGrpsToPublish.erase(vit);

#ifdef GEN_EXCH_DEBUG
			std::cerr << "RsGenExchange::publishGrps() ok -> pushing to notifies"
					  << std::endl;
#endif

			// add to published to allow acknowledgement
			toNotify.insert(std::make_pair(token,
					GrpNote(true,grpId)));
        }
    }

    std::map<uint32_t, GrpNote>::iterator mit = toNotify.begin();

    std::list<RsGxsGroupId> grpChanged;
    for(; mit != toNotify.end(); mit++)
    {
    	GrpNote& note = mit->second;
    	uint8_t status = note.first ? RsTokenService::GXS_REQUEST_V2_STATUS_COMPLETE
    			: RsTokenService::GXS_REQUEST_V2_STATUS_FAILED;

    	mGrpNotify.insert(std::make_pair(mit->first, note.second));
		mDataAccess->updatePublicRequestStatus(mit->first, status);

		if(note.first)
			grpChanged.push_back(note.second);
    }

    if(!grpChanged.empty())
    {
    	RsGxsGroupChange* gc = new RsGxsGroupChange(RsGxsNotify::TYPE_PUBLISH, false);
    	gc->mGrpIdList = grpChanged;
    	mNotifications.push_back(gc);
    }

}



uint32_t RsGenExchange::generatePublicToken()
{
    return mDataAccess->generatePublicToken();
}

bool RsGenExchange::updatePublicRequestStatus(const uint32_t &token, const uint32_t &status)
{
    return mDataAccess->updatePublicRequestStatus(token, status);
}

bool RsGenExchange::disposeOfPublicToken(const uint32_t &token)
{
    return mDataAccess->disposeOfPublicToken(token);
}

RsGeneralDataService* RsGenExchange::getDataStore()
{
    return mDataStore;
}

bool RsGenExchange::getGroupKeys(const RsGxsGroupId &grpId, RsTlvSecurityKeySet &keySet)
{
    if(grpId.isNull())
        return false;

    RsStackMutex stack(mGenMtx);

    std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMeta;
    grpMeta[grpId] = NULL;
    mDataStore->retrieveGxsGrpMetaData(grpMeta);

    if(grpMeta.empty())
        return false;

    RsGxsGrpMetaData* meta = grpMeta[grpId];

    if(meta == NULL)
        return false;

    keySet = meta->keys;
    return true;
}

void RsGenExchange::processRecvdData()
{
    processRecvdGroups();

    processRecvdMessages();

    performUpdateValidation();
}


void RsGenExchange::computeHash(const RsTlvBinaryData& data, RsFileHash& hash)
{
	pqihash pHash;
	pHash.addData(data.bin_data, data.bin_len);
	pHash.Complete(hash);
}

void RsGenExchange::processRecvdMessages()
{
    RsStackMutex stack(mGenMtx);

#ifdef GEN_EXCH_DEBUG
	 if(!mMsgPendingValidate.empty())
		 std::cerr << "processing received messages" << std::endl;
#endif
    NxsMsgPendingVect::iterator pend_it = mMsgPendingValidate.begin();

#ifdef GEN_EXCH_DEBUG
	 if(!mMsgPendingValidate.empty())
		 std::cerr << "  pending validation" << std::endl;
#endif
    for(; pend_it != mMsgPendingValidate.end();)
    {
    	GxsPendingItem<RsNxsMsg*, RsGxsGrpMsgIdPair>& gpsi = *pend_it;

#ifdef GEN_EXCH_DEBUG
        std::cerr << "    grp=" << gpsi.mId.first << ", msg=" << gpsi.mId.second << ", attempts=" << gpsi.mAttempts ;
#endif
    	if(gpsi.mAttempts == VALIDATE_MAX_ATTEMPTS)
    	{
#ifdef GEN_EXCH_DEBUG
			std::cerr << " = max! deleting." << std::endl;
#endif
    		delete gpsi.mItem;
    		pend_it = mMsgPendingValidate.erase(pend_it);
    	}
    	else
    	{
#ifdef GEN_EXCH_DEBUG
			std::cerr << " movign to recvd." << std::endl;
#endif
    		mReceivedMsgs.push_back(gpsi.mItem);
    		pend_it++;
    	}
    }

    if(mReceivedMsgs.empty())
        return;

    std::vector<RsNxsMsg*>::iterator vit = mReceivedMsgs.begin();
    GxsMsgReq msgIds;
    std::map<RsNxsMsg*, RsGxsMsgMetaData*> msgs;

    std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetas;

    // coalesce group meta retrieval for performance
    for(; vit != mReceivedMsgs.end(); vit++)
    {
        RsNxsMsg* msg = *vit;
        grpMetas.insert(std::make_pair(msg->grpId, (RsGxsGrpMetaData*)NULL));
    }

    mDataStore->retrieveGxsGrpMetaData(grpMetas);

#ifdef GEN_EXCH_DEBUG
	 std::cerr << "  updating received messages:" << std::endl;
#endif
    for(vit = mReceivedMsgs.begin(); vit != mReceivedMsgs.end(); vit++)
    {
        RsNxsMsg* msg = *vit;
        RsGxsMsgMetaData* meta = new RsGxsMsgMetaData();

        bool ok = false;

        if(msg->meta.bin_len != 0)
        	ok = meta->deserialise(msg->meta.bin_data, &(msg->meta.bin_len));

        msg->metaData = meta;

#ifdef GEN_EXCH_DEBUG
		  std::cerr << "    deserialised info: grp id=" << meta->mGroupId << ", msg id=" << meta->mMsgId ;
#endif
        uint8_t validateReturn = VALIDATE_FAIL;

        if(ok)
        {
            std::map<RsGxsGroupId, RsGxsGrpMetaData*>::iterator mit = grpMetas.find(msg->grpId);

#ifdef GEN_EXCH_DEBUG
				std::cerr << "    msg info         : grp id=" << msg->grpId << ", msg id=" << msg->msgId << std::endl;
#endif

            // validate msg
            if(mit != grpMetas.end())
            {
                RsGxsGrpMetaData* grpMeta = mit->second;
                validateReturn = validateMsg(msg, grpMeta->mGroupFlags, grpMeta->keys);
#ifdef GEN_EXCH_DEBUG
					 std::cerr << "    message validation result: " << validateReturn << std::endl;
#endif
            }

            if(validateReturn == VALIDATE_SUCCESS)
            {
                meta->mMsgStatus = GXS_SERV::GXS_MSG_STATUS_UNPROCESSED | GXS_SERV::GXS_MSG_STATUS_UNREAD;
                msgs.insert(std::make_pair(msg, meta));
                msgIds[msg->grpId].push_back(msg->msgId);

                NxsMsgPendingVect::iterator validated_entry = std::find(mMsgPendingValidate.begin(), mMsgPendingValidate.end(),
                		getMsgIdPair(*msg));

                if(validated_entry != mMsgPendingValidate.end()) mMsgPendingValidate.erase(validated_entry);

                computeHash(msg->msg, meta->mHash);
                meta->recvTS = time(NULL);
#ifdef GEN_EXCH_DEBUG
					 std::cerr << "    new status flags: " << meta->mMsgStatus << std::endl;
					 std::cerr << "    computed hash: " << meta->mHash << std::endl;
#endif
            }
        }
        else
        {
#ifdef GEN_EXCH_DEBUG
			  std::cerr << " deserialisation failed!" <<std::endl;
#endif
        	validateReturn = VALIDATE_FAIL;
        }

        if(validateReturn == VALIDATE_FAIL)
        {

#ifdef GEN_EXCH_DEBUG
            std::cerr << "failed to deserialise incoming meta, msgId: "
            		<< "msg->grpId: " << msg->grpId << ", msgId: " << msg->msgId << std::endl;
#endif

            NxsMsgPendingVect::iterator failed_entry = std::find(mMsgPendingValidate.begin(), mMsgPendingValidate.end(),
            		getMsgIdPair(*msg));

            if(failed_entry != mMsgPendingValidate.end()) mMsgPendingValidate.erase(failed_entry);
			delete msg;


        }
        else if(validateReturn == VALIDATE_FAIL_TRY_LATER)
        {

#ifdef GEN_EXCH_DEBUG
            std::cerr << "failed to validate msg, trying again: "
                    << "msg->grpId: " << msg->grpId << ", msgId: " << msg->msgId << std::endl;
#endif

        	RsGxsGrpMsgIdPair id;
        	id.first = msg->grpId;
        	id.second = msg->msgId;

        	// first check you haven't made too many attempts

        	NxsMsgPendingVect::iterator vit = std::find(
        			mMsgPendingValidate.begin(), mMsgPendingValidate.end(), id);

        	if(vit == mMsgPendingValidate.end())
        	{
        		GxsPendingItem<RsNxsMsg*, RsGxsGrpMsgIdPair> item(msg, id);
        		mMsgPendingValidate.push_back(item);
        	}else
        	{
				vit->mAttempts++;
        	}
        }
    }

    // clean up resources from group meta retrieval
    freeAndClearContainerResource<std::map<RsGxsGroupId, RsGxsGrpMetaData*>,
    	RsGxsGrpMetaData*>(grpMetas);

    if(!msgIds.empty())
    {
#ifdef GEN_EXCH_DEBUG
        std::cerr << "  removing existing and old messages from incoming list." << std::endl;
#endif
        removeDeleteExistingMessages(msgs, msgIds);

#ifdef GEN_EXCH_DEBUG
        std::cerr << "  storing remaining messages" << std::endl;
#endif
        mDataStore->storeMessage(msgs);

        RsGxsMsgChange* c = new RsGxsMsgChange(RsGxsNotify::TYPE_RECEIVE, false);
        c->msgChangeMap = msgIds;
        mNotifications.push_back(c);
    }

    mReceivedMsgs.clear();
}

void RsGenExchange::processRecvdGroups()
{
    RsStackMutex stack(mGenMtx);

    if(mReceivedGrps.empty())
        return;

    NxsGrpPendValidVect::iterator vit = mReceivedGrps.begin();
    std::vector<RsGxsGroupId> existingGrpIds;
    std::list<RsGxsGroupId> grpIds;

    std::map<RsNxsGrp*, RsGxsGrpMetaData*> grps;

    mDataStore->retrieveGroupIds(existingGrpIds);

    while( vit != mReceivedGrps.end())
    {
    	GxsPendingItem<RsNxsGrp*, RsGxsGroupId>& gpsi = *vit;
        RsNxsGrp* grp = gpsi.mItem;
        RsGxsGrpMetaData* meta = new RsGxsGrpMetaData();
        bool deserialOk = false;

        if(grp->meta.bin_len != 0)
        	deserialOk = meta->deserialise(grp->meta.bin_data, grp->meta.bin_len);

        bool erase = true;

        if(deserialOk)
        {
        	grp->metaData = meta;
        	uint8_t ret = validateGrp(grp);


        	if(ret == VALIDATE_SUCCESS)
        	{
				meta->mGroupStatus = GXS_SERV::GXS_GRP_STATUS_UNPROCESSED | GXS_SERV::GXS_GRP_STATUS_UNREAD;
				meta->mSubscribeFlags = GXS_SERV::GROUP_SUBSCRIBE_NOT_SUBSCRIBED;

				computeHash(grp->grp, meta->mHash);

				// now check if group already existss
				if(std::find(existingGrpIds.begin(), existingGrpIds.end(), grp->grpId) == existingGrpIds.end())
				{
					meta->mRecvTS = time(NULL);
					if(meta->mCircleType == GXS_CIRCLE_TYPE_YOUREYESONLY)
						meta->mOriginator = grp->PeerId();

					grps.insert(std::make_pair(grp, meta));
					grpIds.push_back(grp->grpId);
				}
				else
				{
					GroupUpdate update;
					update.newGrp = grp;
					mGroupUpdates.push_back(update);
				}
				erase = true;
        	}
        	else if(ret == VALIDATE_FAIL)
        	{
#ifdef GEN_EXCH_DEBUG
				std::cerr << "failed to deserialise incoming meta, grpId: "
						<< grp->grpId << std::endl;
#endif
				delete grp;
				erase = true;
        	}
        	else  if(ret == VALIDATE_FAIL_TRY_LATER)
        	{

#ifdef GEN_EXCH_DEBUG
				std::cerr << "failed to validate incoming grp, trying again. grpId: "
						<< grp->grpId << std::endl;
#endif

        		if(gpsi.mAttempts == VALIDATE_MAX_ATTEMPTS)
        		{
        			delete grp;
        			erase = true;
        		}
        		else
        		{
        			erase = false;
        		}
        	}
        }
        else
        {
        	delete grp;
			delete meta;
			erase = true;
        }

        if(erase)
        	vit = mReceivedGrps.erase(vit);
        else
        	vit++;
    }

    if(!grpIds.empty())
    {
        RsGxsGroupChange* c = new RsGxsGroupChange(RsGxsNotify::TYPE_RECEIVE, false);
        c->mGrpIdList = grpIds;
        mNotifications.push_back(c);
        mDataStore->storeGroup(grps);
    }
}

void RsGenExchange::performUpdateValidation()
{
	RsStackMutex stack(mGenMtx);

	if(mGroupUpdates.empty())
		return;

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::performUpdateValidation() " << std::endl;
#endif

	std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetas;

	std::vector<GroupUpdate>::iterator vit = mGroupUpdates.begin();
	for(; vit != mGroupUpdates.end(); vit++)
		grpMetas.insert(std::make_pair(vit->newGrp->grpId, (RsGxsGrpMetaData*)NULL));

	if(!grpMetas.empty())
		mDataStore->retrieveGxsGrpMetaData(grpMetas);
	else
		return;

	vit = mGroupUpdates.begin();
	for(; vit != mGroupUpdates.end(); vit++)
	{
		GroupUpdate& gu = *vit;
		std::map<RsGxsGroupId, RsGxsGrpMetaData*>::iterator mit =
				grpMetas.find(gu.newGrp->grpId);
		gu.oldGrpMeta = mit->second;
		gu.validUpdate = updateValid(*(gu.oldGrpMeta), *(gu.newGrp));
	}

#ifdef GEN_EXCH_DEBUG
	std::cerr << "RsGenExchange::performUpdateValidation() " << std::endl;
#endif

	vit = mGroupUpdates.begin();
	std::map<RsNxsGrp*, RsGxsGrpMetaData*> grps;
	for(; vit != mGroupUpdates.end(); vit++)
	{
		GroupUpdate& gu = *vit;

		if(gu.validUpdate)
		{
			if(gu.newGrp->metaData->mCircleType == GXS_CIRCLE_TYPE_YOUREYESONLY)
				gu.newGrp->metaData->mOriginator = gu.newGrp->PeerId();

			grps.insert(std::make_pair(gu.newGrp, gu.newGrp->metaData));
		}
		else
		{
			delete gu.newGrp;
		}

		delete gu.oldGrpMeta;
	}

	mDataStore->updateGroup(grps);
	mGroupUpdates.clear();
}

bool RsGenExchange::updateValid(RsGxsGrpMetaData& oldGrpMeta, RsNxsGrp& newGrp) const
{
	std::map<SignType, RsTlvKeySignature>& signSet = newGrp.metaData->signSet.keySignSet;
	std::map<SignType, RsTlvKeySignature>::iterator mit = signSet.find(GXS_SERV::FLAG_AUTHEN_ADMIN);

	if(mit == signSet.end())
	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "RsGenExchange::updateValid() new admin sign not found! " << std::endl;
		std::cerr << "RsGenExchange::updateValid() grpId: " << oldGrpMeta.mGroupId << std::endl;
#endif

		return false;
	}

	RsTlvKeySignature adminSign = mit->second;

	std::map<RsGxsId, RsTlvSecurityKey>& keys = oldGrpMeta.keys.keys;
	std::map<RsGxsId, RsTlvSecurityKey>::iterator keyMit = keys.find(RsGxsId(oldGrpMeta.mGroupId));

	if(keyMit == keys.end())
	{
#ifdef GEN_EXCH_DEBUG
		std::cerr << "RsGenExchange::updateValid() admin key not found! " << std::endl;
#endif
		return false;
	}

	// also check this is the latest published group
	bool latest = newGrp.metaData->mPublishTs > oldGrpMeta.mPublishTs;

	return GxsSecurity::validateNxsGrp(newGrp, adminSign, keyMit->second) && latest;
}

void RsGenExchange::setGroupReputationCutOff(uint32_t& token, const RsGxsGroupId& grpId, int CutOff)
{
    RsStackMutex stack(mGenMtx);
    token = mDataAccess->generatePublicToken();

    GrpLocMetaData g;
    g.grpId = grpId;
    g.val.put(RsGeneralDataService::GRP_META_CUTOFF_LEVEL, (int32_t)CutOff);
    mGrpLocMetaMap.insert(std::make_pair(token, g));
}

void RsGenExchange::removeDeleteExistingMessages( RsGeneralDataService::MsgStoreMap& msgs, GxsMsgReq& msgIdsNotify) 
{
	// first get grp ids of messages to be stored

	RsGxsGroupId::std_set mGrpIdsUnique;

	for(RsGeneralDataService::MsgStoreMap::const_iterator cit = msgs.begin(); cit != msgs.end(); cit++)
		mGrpIdsUnique.insert(cit->second->mGroupId);

	//RsGxsGroupId::std_list grpIds(mGrpIdsUnique.begin(), mGrpIdsUnique.end());
	//RsGxsGroupId::std_list::const_iterator it = grpIds.begin();
	typedef std::map<RsGxsGroupId, RsGxsMessageId::std_vector> MsgIdReq;
	MsgIdReq msgIdReq;

	// now get a list of all msgs ids for each group
	for(RsGxsGroupId::std_set::const_iterator it(mGrpIdsUnique.begin()); it != mGrpIdsUnique.end(); it++)
	{
		mDataStore->retrieveMsgIds(*it, msgIdReq[*it]);

#ifdef GEN_EXCH_DEBUG
		const std::vector<RsGxsMessageId>& vec(msgIdReq[*it]) ;
		std::cerr << "  retrieved " << vec.size() << " message ids for group " << *it << std::endl;

		for(uint32_t i=0;i<vec.size();++i)
			std::cerr << "    " << vec[i] << std::endl;
#endif
	}

	//RsGeneralDataService::MsgStoreMap::iterator cit2 = msgs.begin();
	RsGeneralDataService::MsgStoreMap filtered;

	// now for each msg to be stored that exist in the retrieved msg/grp "index" delete and erase from map
	for(RsGeneralDataService::MsgStoreMap::iterator cit2 = msgs.begin(); cit2 != msgs.end(); cit2++)
	{
		const RsGxsMessageId::std_vector& msgIds = msgIdReq[cit2->second->mGroupId];

		std::cerr << "    grpid=" << cit2->second->mGroupId << ", msgid=" << cit2->second->mMsgId ;

		// Avoid storing messages that are already in the database, as well as messages that are too old (or generally do not pass the database storage test)
		//
		if(std::find(msgIds.begin(), msgIds.end(), cit2->second->mMsgId) == msgIds.end() && messagePublicationTest(*cit2->second))
		{
			// passes tests, so add to filtered list
			//
			filtered.insert(*cit2);
#ifdef GEN_EXCH_DEBUG
			std::cerr << "    keeping " << cit2->second->mMsgId << std::endl;
#endif
		}
		else	// remove message from list
		{
			// msg exist in retrieved index
			delete cit2->first;
			RsGxsMessageId::std_vector& notifyIds = msgIdsNotify[cit2->second->mGroupId];
			RsGxsMessageId::std_vector::iterator it2 = std::find(notifyIds.begin(),
					notifyIds.end(), cit2->second->mMsgId);
			if(it2 != notifyIds.end())
				notifyIds.erase(it2);
#ifdef GEN_EXCH_DEBUG
			std::cerr << "    discarding " << cit2->second->mMsgId << std::endl;
#endif
		}
	}

	msgs = filtered;
}

