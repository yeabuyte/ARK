/*
* This source file is part of ArkGameFrame
* For the latest info, see https://github.com/ArkGame
*
* Copyright (c) 2013-2018 ArkGame authors.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
*/

#include "AFCBryNetServer.h"
#include <string.h>

#if ARK_PLATFORM == PLATFORM_WIN
#include <WS2tcpip.h>
#include <winsock2.h>
#pragma  comment(lib,"Ws2_32.lib")
#pragma  comment(lib,"event.lib")
#pragma  comment(lib,"event_core.lib")
#elif ARK_PLATFORM == PLATFORM_APPLE
#include <arpa/inet.h>
#endif

#include <atomic>
#include <memory>

void AFCBryNetServer::Update()
{
    ProcessMsgLogicThread();
}

int AFCBryNetServer::Start(const unsigned int nMaxClient, const std::string& strAddrPort, const int nServerID, const int nThreadCount)
{
    std::string strHost;
    int port;
    SplitHostPort(strAddrPort, strHost, port);
    m_plistenThread->startListen(false, strHost, port, std::bind(&AFCBryNetServer::OnAcceptConnectionInner, this, std::placeholders::_1));

    m_pServer->startWorkThread(nThreadCount);
    return 0;
}


size_t AFCBryNetServer::OnMessageInner(const brynet::net::TCPSession::PTR& session, const char* buffer, size_t len)
{
    const auto ud = brynet::net::cast<brynet::net::TcpService::SESSION_TYPE>(session->getUD());
    AFGUID xClient(0, (uint64_t)ud);

    AFScopeRdLock xGuard(mRWLock);

    auto xFind = mmObject.find(xClient);
    if(xFind == mmObject.end())
    {
        return len;
    }

    xFind->second->AddBuff(buffer, len);
    DismantleNet(xFind->second);
    return len;
}

void AFCBryNetServer::OnAcceptConnectionInner(brynet::net::TcpSocket::PTR socket)
{
    socket->SocketNodelay();
    m_pServer->addSession(std::move(socket),
                          brynet::net::AddSessionOption::WithEnterCallback(std::bind(&AFCBryNetServer::OnClientConnectionInner, this, std::placeholders::_1)),
                          brynet::net::AddSessionOption::WithMaxRecvBufferSize(1024 * 1024));
}

void AFCBryNetServer::OnClientConnectionInner(const brynet::net::TCPSession::PTR & session)
{
    session->setDataCallback(std::bind(&AFCBryNetServer::OnMessageInner, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    session->setDisConnectCallback(std::bind(&AFCBryNetServer::OnClientDisConnectionInner, this, std::placeholders::_1));

    MsgFromBryNetInfo* pMsg = new MsgFromBryNetInfo(session);
    const auto ud = brynet::net::cast<brynet::net::TcpService::SESSION_TYPE>(session->getUD());
    pMsg->xClientID.nLow = (uint64_t)ud;
    pMsg->nType = CONNECTED;
    {
        AFScopeWrLock xGuard(mRWLock);

        BryNetObject* pEntity = new BryNetObject(this, pMsg->xClientID, session);
        if(AddNetObject(pMsg->xClientID, pEntity))
        {
            pEntity->mqMsgFromNet.Push(pMsg);
        }
    }
}

void AFCBryNetServer::OnClientDisConnectionInner(const brynet::net::TCPSession::PTR & session)
{
    const auto ud = brynet::net::cast<brynet::net::TcpService::SESSION_TYPE>(session->getUD());
    AFGUID xClient(0, (uint64_t)ud);
    AFScopeWrLock xGuard(mRWLock);

    auto xFind = mmObject.find(xClient);
    if(xFind == mmObject.end())
    {
        return ;
    }

    MsgFromBryNetInfo* pMsg = new MsgFromBryNetInfo(session);
    pMsg->xClientID = xClient;
    pMsg->nType = DISCONNECTED;

    xFind->second->mqMsgFromNet.Push(pMsg);
}

bool AFCBryNetServer::SplitHostPort(const std::string& strIpPort, std::string& host, int& port)
{
    std::string a = strIpPort;
    if(a.empty())
    {
        return false;
    }

    size_t index = a.rfind(':');
    if(index == std::string::npos)
    {
        return false;
    }

    if(index == a.size() - 1)
    {
        return false;
    }

    port = std::atoi(&a[index + 1]);

    host = std::string(strIpPort, 0, index);
    if(host[0] == '[')
    {
        if(*host.rbegin() != ']')
        {
            return false;
        }

        // trim the leading '[' and trail ']'
        host = std::string(host.data() + 1, host.size() - 2);
    }

    // Compatible with "fe80::886a:49f3:20f3:add2]:80"
    if(*host.rbegin() == ']')
    {
        // trim the trail ']'
        host = std::string(host.data(), host.size() - 1);
    }

    return true;
}

void AFCBryNetServer::ProcessMsgLogicThread()
{
    std::list<AFGUID> xNeedRemoveList;
    {
        AFScopeRdLock xGuard(mRWLock);
        for(std::map<AFGUID, BryNetObject*>::iterator iter = mmObject.begin(); iter != mmObject.end(); ++iter)
        {
            ProcessMsgLogicThread(iter->second);
            if(!iter->second->NeedRemove())
            {
                continue;
            }

            xNeedRemoveList.push_back(iter->second->GetClientID());
        }
    }

    for(std::list<AFGUID>::iterator iter = xNeedRemoveList.begin(); iter != xNeedRemoveList.end(); ++iter)
    {
        AFScopeWrLock xGuard(mRWLock);
        RemoveNetObject(*iter);
    }
}

void AFCBryNetServer::ProcessMsgLogicThread(BryNetObject* pEntity)
{
    //Handle Msg
    size_t nReceiveCount = pEntity->mqMsgFromNet.Count();
    for(size_t i = 0; i < nReceiveCount; ++i)
    {
        MsgFromBryNetInfo* pMsgFromNet(NULL);
        if(!pEntity->mqMsgFromNet.Pop(pMsgFromNet))
        {
            break;
        }

        if(pMsgFromNet == nullptr)
        {
            continue;
        }

        switch(pMsgFromNet->nType)
        {
        case RECIVEDATA:
            {
                int nRet = 0;
                if(mRecvCB)
                {
                    mRecvCB(pMsgFromNet->xHead, pMsgFromNet->xHead.GetMsgID(), pMsgFromNet->strMsg.c_str(), pMsgFromNet->strMsg.size(), pEntity->GetClientID());
                }
            }
            break;
        case CONNECTED:
            {
                mEventCB((NetEventType)pMsgFromNet->nType, pMsgFromNet->xClientID, mnServerID);
            }
            break;
        case DISCONNECTED:
            {
                mEventCB((NetEventType)pMsgFromNet->nType, pMsgFromNet->xClientID, mnServerID);
                pEntity->SetNeedRemove(true);
            }
            break;
        default:
            break;
        }

        delete pMsgFromNet;
    }
}

bool AFCBryNetServer::Final()
{
    bWorking = false;
    return true;
}

bool AFCBryNetServer::SendMsgToAllClient(const char* msg, const size_t nLen)
{
    std::map<AFGUID, BryNetObject*>::iterator it = mmObject.begin();
    for(; it != mmObject.end(); ++it)
    {
        BryNetObject* pNetObject = (BryNetObject*)it->second;
        if(pNetObject && !pNetObject->NeedRemove())
        {
            pNetObject->GetConnPtr()->send(msg, nLen);
        }
    }

    return true;
}

bool AFCBryNetServer::SendMsg(const char* msg, const size_t nLen, const AFGUID& xClient)
{
    AFScopeRdLock xGuard(mRWLock);

    BryNetObject* pNetObject = GetNetObject(xClient);
    if(pNetObject == nullptr)
    {
        return false;
    }

    pNetObject->GetConnPtr()->send(msg, nLen);
    return true;
}

bool AFCBryNetServer::AddNetObject(const AFGUID& xClientID, BryNetObject* pEntity)
{
    return mmObject.insert(std::make_pair(xClientID, pEntity)).second;
}

bool AFCBryNetServer::RemoveNetObject(const AFGUID& xClientID)
{
    BryNetObject* pNetObject = GetNetObject(xClientID);
    if(pNetObject)
    {
        delete pNetObject;
    }
    return mmObject.erase(xClientID);
}

bool AFCBryNetServer::CloseNetObject(const AFGUID& xClientID)
{
    BryNetObject* pEntity = GetNetObject(xClientID);
    if(pEntity)
    {
        pEntity->GetConnPtr()->postDisConnect();
    }

    return true;
}

bool AFCBryNetServer::DismantleNet(BryNetObject* pEntity)
{
    for(; pEntity->GetBuffLen() >= AFIMsgHead::ARK_MSG_HEAD_LENGTH;)
    {
        AFCMsgHead xHead;
        int nMsgBodyLength = DeCode(pEntity->GetBuff(), pEntity->GetBuffLen(), xHead);
        if(nMsgBodyLength >= 0 && xHead.GetMsgID() > 0)
        {
            MsgFromBryNetInfo* pNetInfo = new  MsgFromBryNetInfo(pEntity->GetConnPtr());
            pNetInfo->xHead = xHead;
            pNetInfo->nType = RECIVEDATA;
            pNetInfo->strMsg.append(pEntity->GetBuff() + AFIMsgHead::ARK_MSG_HEAD_LENGTH, nMsgBodyLength);
            pEntity->mqMsgFromNet.Push(pNetInfo);
            size_t nRet = pEntity->RemoveBuff(nMsgBodyLength + AFIMsgHead::ARK_MSG_HEAD_LENGTH);
        }
        else
        {
            break;
        }
    }

    return true;
}

bool AFCBryNetServer::CloseSocketAll()
{
    std::map<AFGUID, BryNetObject*>::iterator it = mmObject.begin();
    for(it; it != mmObject.end(); ++it)
    {
        it->second->GetConnPtr()->postDisConnect();
        delete it->second;
    }

    mmObject.clear();

    return true;
}

BryNetObject* AFCBryNetServer::GetNetObject(const AFGUID& xClientID)
{
    std::map<AFGUID, BryNetObject*>::iterator it = mmObject.find(xClientID);
    if(it != mmObject.end())
    {
        return it->second;
    }

    return NULL;
}

bool AFCBryNetServer::SendMsgWithOutHead(const uint16_t nMsgID, const char* msg, const size_t nLen, const AFGUID& xClientID, const AFGUID& xPlayerID)
{
    std::string strOutData;
    AFCMsgHead xHead;
    xHead.SetMsgID(nMsgID);
    xHead.SetPlayerID(xPlayerID);
    xHead.SetBodyLength(nLen);

    int nAllLen = EnCode(xHead, msg, nLen, strOutData);
    if(nAllLen == nLen + AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return SendMsg(strOutData.c_str(), strOutData.length(), xClientID);
    }

    return false;
}

bool AFCBryNetServer::SendMsgToAllClientWithOutHead(const uint16_t nMsgID, const char* msg, const size_t nLen, const AFGUID& xPlayerID)
{
    std::string strOutData;
    AFCMsgHead xHead;
    xHead.SetMsgID(nMsgID);
    xHead.SetPlayerID(xPlayerID);

    int nAllLen = EnCode(xHead, msg, nLen, strOutData);
    if(nAllLen == nLen + AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return SendMsgToAllClient(strOutData.c_str(), strOutData.length());
    }

    return false;
}

int AFCBryNetServer::EnCode(const AFCMsgHead& xHead, const char* strData, const size_t len, std::string& strOutData)
{
    char szHead[AFIMsgHead::ARK_MSG_HEAD_LENGTH] = { 0 };
    int nRet = xHead.EnCode(szHead);

    strOutData.clear();
    strOutData.append(szHead, AFIMsgHead::ARK_MSG_HEAD_LENGTH);
    strOutData.append(strData, len);

    return xHead.GetBodyLength() + AFIMsgHead::ARK_MSG_HEAD_LENGTH;
}

int AFCBryNetServer::DeCode(const char* strData, const size_t len, AFCMsgHead& xHead)
{
    if(len < AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return -1;
    }

    if(AFIMsgHead::ARK_MSG_HEAD_LENGTH != xHead.DeCode(strData))
    {
        return -2;
    }

    if(xHead.GetBodyLength() > (len - AFIMsgHead::ARK_MSG_HEAD_LENGTH))
    {
        return -3;
    }

    return xHead.GetBodyLength();
}

