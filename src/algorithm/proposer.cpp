/*
Tencent is pleased to support the open source community by making 
PhxPaxos available.
Copyright (C) 2016 THL A29 Limited, a Tencent company. 
All rights reserved.

Licensed under the BSD 3-Clause License (the "License"); you may 
not use this file except in compliance with the License. You may 
obtain a copy of the License at

https://opensource.org/licenses/BSD-3-Clause

Unless required by applicable law or agreed to in writing, software 
distributed under the License is distributed on an "AS IS" basis, 
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or 
implied. See the License for the specific language governing 
permissions and limitations under the License.

See the AUTHORS file for names of contributors. 
*/

#include "proposer.h"
#include "learner.h"
#include "phxpaxos/sm.h"
#include "instance.h"

namespace phxpaxos
{

// ��ס proposalID �Ǵ� 1 ��ʼ�ġ�
ProposerState :: ProposerState(const Config * poConfig)
{
    m_poConfig = (Config *)poConfig;
    m_llProposalID = 1;
    Init();
}

ProposerState :: ~ProposerState()
{
}

void ProposerState :: Init()
{
    m_llHighestOtherProposalID = 0;
    m_sValue.clear();
}

void ProposerState ::  SetStartProposalID(const uint64_t llProposalID)
{
    m_llProposalID = llProposalID;
}

void ProposerState :: NewPrepare()
{
    PLGHead("START ProposalID %lu HighestOther %lu MyNodeID %lu",
            m_llProposalID, m_llHighestOtherProposalID, m_poConfig->GetMyNodeID());
     
    // �ٴ��·��� proposal ʱ������Ҫȷ���Լ��� Id ���������Ѿ���֪����
    // �� ID �����ֵ�������ȣ���һ���ɡ�
    // ID �����ʱ������������ڵ�����˳�ͻ��
    // ���ﲻ�õ��� ID �ظ����������Ϊ ballot ID ���� nodeID ��
    uint64_t llMaxProposalID =
        m_llProposalID > m_llHighestOtherProposalID ? m_llProposalID : m_llHighestOtherProposalID;

    m_llProposalID = llMaxProposalID + 1;

    PLGHead("END New.ProposalID %lu", m_llProposalID);

}

void ProposerState :: AddPreAcceptValue(
        const BallotNumber & oOtherPreAcceptBallot, 
        const std::string & sOtherPreAcceptValue)
{
    PLGDebug("OtherPreAcceptID %lu OtherPreAcceptNodeID %lu HighestOtherPreAcceptID %lu "
            "HighestOtherPreAcceptNodeID %lu OtherPreAcceptValue %zu",
            oOtherPreAcceptBallot.m_llProposalID, oOtherPreAcceptBallot.m_llNodeID,
            m_oHighestOtherPreAcceptBallot.m_llProposalID, m_oHighestOtherPreAcceptBallot.m_llNodeID, 
            sOtherPreAcceptValue.size());

    if (oOtherPreAcceptBallot.isnull())
    {
        return;
    }

    // �������� ballot ID ֵ��
    if (oOtherPreAcceptBallot > m_oHighestOtherPreAcceptBallot)
    {
        m_oHighestOtherPreAcceptBallot = oOtherPreAcceptBallot;
        m_sValue = sOtherPreAcceptValue;
    }
}

const uint64_t ProposerState :: GetProposalID()
{
    return m_llProposalID;
}

const std::string & ProposerState :: GetValue()
{
    return m_sValue;
}

void ProposerState :: SetValue(const std::string & sValue)
{
    m_sValue = sValue;
}

void ProposerState :: SetOtherProposalID(const uint64_t llOtherProposalID)
{
    if (llOtherProposalID > m_llHighestOtherProposalID)
    {
        m_llHighestOtherProposalID = llOtherProposalID;
    }
}

void ProposerState :: ResetHighestOtherPreAcceptBallot()
{
    m_oHighestOtherPreAcceptBallot.reset();
}

////////////////////////////////////////////////////////////////

Proposer :: Proposer(
        const Config * poConfig, 
        const MsgTransport * poMsgTransport,
        const Instance * poInstance,
        const Learner * poLearner,
        const IOLoop * poIOLoop)
    : Base(poConfig, poMsgTransport, poInstance), m_oProposerState(poConfig), m_oMsgCounter(poConfig)
{
    m_poLearner = (Learner *)poLearner;
    m_poIOLoop = (IOLoop *)poIOLoop;
    
    m_bIsPreparing = false;
    m_bIsAccepting = false;

    m_bCanSkipPrepare = false;

    InitForNewPaxosInstance();

    m_iPrepareTimerID = 0;
    m_iAcceptTimerID = 0;
    m_llTimeoutInstanceID = 0;

    m_iLastPrepareTimeoutMs = m_poConfig->GetPrepareTimeoutMs();
    m_iLastAcceptTimeoutMs = m_poConfig->GetAcceptTimeoutMs();

    m_bWasRejectBySomeone = false;
}

Proposer :: ~Proposer()
{
}

void Proposer :: SetStartProposalID(const uint64_t llProposalID)
{
    m_oProposerState.SetStartProposalID(llProposalID);
}

void Proposer :: InitForNewPaxosInstance()
{
    m_oMsgCounter.StartNewRound();
    m_oProposerState.Init();

    ExitPrepare();
    ExitAccept();
}

bool Proposer :: IsWorking()
{
    return m_bIsPreparing || m_bIsAccepting;
}

int Proposer :: NewValue(const std::string & sValue)
{
    BP->GetProposerBP()->NewProposal(sValue);

    if (m_oProposerState.GetValue().size() == 0)
    {
        m_oProposerState.SetValue(sValue);
    }

    m_iLastPrepareTimeoutMs = START_PREPARE_TIMEOUTMS;
    m_iLastAcceptTimeoutMs = START_ACCEPT_TIMEOUTMS;

    // ����ֱ������ multi-paxos ���Ż���ȥ���� single-paxos �ĵ�һ�׶Ρ�
    if (m_bCanSkipPrepare && !m_bWasRejectBySomeone)
    {
        BP->GetProposerBP()->NewProposalSkipPrepare();

        PLGHead("skip prepare, directly start accept");
        Accept();
    }
    // �����ͻ�ˣ�Ҫ����ִ�� prepare �׶Ρ�
    else
    {
        //if not reject by someone, no need to increase ballot
        Prepare(m_bWasRejectBySomeone);
    }

    return 0;
}

void Proposer :: ExitPrepare()
{
    // �����;�˳���˵���Ѿ���ʱ�ˡ�
    if (m_bIsPreparing)
    {
        m_bIsPreparing = false;
        
        m_poIOLoop->RemoveTimer(m_iPrepareTimerID);
    }
}

void Proposer :: ExitAccept()
{
    if (m_bIsAccepting)
    {
        m_bIsAccepting = false;
        
        m_poIOLoop->RemoveTimer(m_iAcceptTimerID);
    }
}

void Proposer :: AddPrepareTimer(const int iTimeoutMs)
{
    // ����� ID �Ļ��ڶ�ʱ�� map �У�ȥ���Ϳɣ����ö�ʱ�� ID Ϊ0��
    if (m_iPrepareTimerID > 0)
    {
        m_poIOLoop->RemoveTimer(m_iPrepareTimerID);
    }

    // 3.27 : ��ʱ��Ϊ����������� timeout ���µ�ֵ�����¼��ɡ�
    if (iTimeoutMs > 0)
    {
        m_poIOLoop->AddTimer(
                iTimeoutMs,
                Timer_Proposer_Prepare_Timeout,
                m_iPrepareTimerID);
        return;
    }

    // 3.27 : ��������ֵΪ 0 �����ǰ� timeout ֵ�� m_iLastPrepareTimeoutMs 
    // �´�������ʱֱ�ӳ��� 2 ���ٳ�ʱ�Ŀ����ԡ�
    m_poIOLoop->AddTimer(
            m_iLastPrepareTimeoutMs,
            Timer_Proposer_Prepare_Timeout,
            m_iPrepareTimerID);

    m_llTimeoutInstanceID = GetInstanceID();

    PLGHead("timeoutms %d", m_iLastPrepareTimeoutMs);

    // ���µ� prepare timeout Ĭ��ֱֵ�ӳ��� 2�����ǲ��ܳ�����ֵ�� 
    m_iLastPrepareTimeoutMs *= 2;
    if (m_iLastPrepareTimeoutMs > MAX_PREPARE_TIMEOUTMS)
    {
        m_iLastPrepareTimeoutMs = MAX_PREPARE_TIMEOUTMS;
    }
}

void Proposer :: AddAcceptTimer(const int iTimeoutMs)
{
    if (m_iAcceptTimerID > 0)
    {
        m_poIOLoop->RemoveTimer(m_iAcceptTimerID);
    }

    if (iTimeoutMs > 0)
    {
        m_poIOLoop->AddTimer(
                iTimeoutMs,
                Timer_Proposer_Accept_Timeout,
                m_iAcceptTimerID);
        return;
    }

    m_poIOLoop->AddTimer(
            m_iLastAcceptTimeoutMs,
            Timer_Proposer_Accept_Timeout,
            m_iAcceptTimerID);

    m_llTimeoutInstanceID = GetInstanceID();
    
    PLGHead("timeoutms %d", m_iLastPrepareTimeoutMs);

    m_iLastAcceptTimeoutMs *= 2;
    if (m_iLastAcceptTimeoutMs > MAX_ACCEPT_TIMEOUTMS)
    {
        m_iLastAcceptTimeoutMs = MAX_ACCEPT_TIMEOUTMS;
    }
}

void Proposer :: Prepare(const bool bNeedNewBallot)
{
    PLGHead("START Now.InstanceID %lu MyNodeID %lu State.ProposalID %lu State.ValueLen %zu",
            GetInstanceID(), m_poConfig->GetMyNodeID(), m_oProposerState.GetProposalID(),
            m_oProposerState.GetValue().size());

    BP->GetProposerBP()->Prepare();
    m_oTimeStat.Point();
    
    ExitAccept();
    m_bIsPreparing = true;
    // �������������������������һ�׶Ρ�
    m_bCanSkipPrepare = false;
    m_bWasRejectBySomeone = false;

    // �������ֵѡ������ͻ���µ� ballotID ��ѡ����
    m_oProposerState.ResetHighestOtherPreAcceptBallot();
    if (bNeedNewBallot)
    {
        m_oProposerState.NewPrepare();
    }

    PaxosMsg oPaxosMsg;
    oPaxosMsg.set_msgtype(MsgType_PaxosPrepare);
    oPaxosMsg.set_instanceid(GetInstanceID());
    oPaxosMsg.set_nodeid(m_poConfig->GetMyNodeID());
    oPaxosMsg.set_proposalid(m_oProposerState.GetProposalID());

    // ���� accept �ɹ�����ʧ�ܣ����Ƕ���ʼ�µ�һ�ּ�����
    m_oMsgCounter.StartNewRound();

    // ����ǰ�� prepare ���뵽��ʱ������ȥ��
    AddPrepareTimer();

    PLGHead("END OK");

    // �㲥�����еĽڵ㳢�� prepare ��
    BroadcastMessage(oPaxosMsg);
}

void Proposer :: OnPrepareReply(const PaxosMsg & oPaxosMsg)
{
    PLGHead("START Msg.ProposalID %lu State.ProposalID %lu Msg.from_nodeid %lu RejectByPromiseID %lu",
            oPaxosMsg.proposalid(), m_oProposerState.GetProposalID(), 
            oPaxosMsg.nodeid(), oPaxosMsg.rejectbypromiseid());

    BP->GetProposerBP()->OnPrepareReply();

    // �յ���Ϣʱ�����Ѿ����� prepare �׶��ˣ�ֱ�Ӻ��������Ϣ��
    if (!m_bIsPreparing)
    {
        BP->GetProposerBP()->OnPrepareReplyButNotPreparing();
        //PLGErr("Not preparing, skip this msg");
        return;
    }

    // ��Ȼ���� prepare �׶Σ����� proposeID ��һ�£�ͬ�����ԡ�
    if (oPaxosMsg.proposalid() != m_oProposerState.GetProposalID())
    {
        BP->GetProposerBP()->OnPrepareReplyNotSameProposalIDMsg();
        //PLGErr("ProposalID not same, skip this msg");
        return;
    }

    // ͳ�ƻظ��Ľڵ�������
    m_oMsgCounter.AddReceive(oPaxosMsg.nodeid());

    if (oPaxosMsg.rejectbypromiseid() == 0)
    {
        BallotNumber oBallot(oPaxosMsg.preacceptid(), oPaxosMsg.preacceptnodeid());
        PLGDebug("[Promise] PreAcceptedID %lu PreAcceptedNodeID %lu ValueSize %zu", 
                oPaxosMsg.preacceptid(), oPaxosMsg.preacceptnodeid(), oPaxosMsg.value().size());
        // ͳ���޳ɵĽڵ�������
        m_oMsgCounter.AddPromiseOrAccept(oPaxosMsg.nodeid());
        m_oProposerState.AddPreAcceptValue(oBallot, oPaxosMsg.value());
    }
    else
    {
        PLGDebug("[Reject] RejectByPromiseID %lu", oPaxosMsg.rejectbypromiseid());
        
        // ͳ�ƾܾ��Ľڵ�������
        m_oMsgCounter.AddReject(oPaxosMsg.nodeid());
        m_bWasRejectBySomeone = true;
        m_oProposerState.SetOtherProposalID(oPaxosMsg.rejectbypromiseid());
    }

    // ����������ͬ��ζ�ű��� prepare �׶γɹ���
    if (m_oMsgCounter.IsPassedOnThisRound())
    {
        int iUseTimeMs = m_oTimeStat.Point();
        BP->GetProposerBP()->PreparePass(iUseTimeMs);
        PLGImp("[Pass] start accept, usetime %dms", iUseTimeMs);

        // 3.21 : �´��ٴ����� proposer ʱ������Ҫ�ٽ��� prepare �׶��ˡ�
        // �������˻���ΪʲôҪ��������Ϊ�ڵȴ� accept �ظ��Ĺ����У�
        // ��ǰ�̻߳������ӽ� loop �У��ٴλ�����Ҫһ����־λ�жϡ�
        m_bCanSkipPrepare = true;
        Accept();
    }
    // 3.21 : �յ�������ڵ� reject ����Ϣ�����Ѿ��յ����յ������нڵ����Ϣ��
    // ����һ������Ķ�ʱ����Ϊ�������Ľڵ�����ͻ��
    else if (m_oMsgCounter.IsRejectedOnThisRound()
            || m_oMsgCounter.IsAllReceiveOnThisRound())
    {
        BP->GetProposerBP()->PrepareNotPass();
        PLGImp("[Not Pass] wait 30ms and restart prepare");
        AddPrepareTimer(OtherUtils::FastRand() % 30 + 10);
    }

    PLGHead("END");
}

void Proposer :: OnExpiredPrepareReply(const PaxosMsg & oPaxosMsg)
{
    // �����Լ��� proposalID ֵ��������һ�������Ż���
    if (oPaxosMsg.rejectbypromiseid() != 0)
    {
        PLGDebug("[Expired Prepare Reply Reject] RejectByPromiseID %lu", oPaxosMsg.rejectbypromiseid());
        m_bWasRejectBySomeone = true;
        m_oProposerState.SetOtherProposalID(oPaxosMsg.rejectbypromiseid());
    }
}

void Proposer :: Accept()
{
    PLGHead("START ProposalID %lu ValueSize %zu ValueLen %zu", 
            m_oProposerState.GetProposalID(), m_oProposerState.GetValue().size(), m_oProposerState.GetValue().size());

    BP->GetProposerBP()->Accept();
    m_oTimeStat.Point();

    // �Ѿ����� accept ״̬����� prepare �׶εı�־λ�Ͷ�ʱ����
    ExitPrepare();
    m_bIsAccepting = true;
    
    PaxosMsg oPaxosMsg;
    oPaxosMsg.set_msgtype(MsgType_PaxosAccept);
    oPaxosMsg.set_instanceid(GetInstanceID());
    oPaxosMsg.set_nodeid(m_poConfig->GetMyNodeID());
    oPaxosMsg.set_proposalid(m_oProposerState.GetProposalID());
    oPaxosMsg.set_value(m_oProposerState.GetValue());
    oPaxosMsg.set_lastchecksum(GetLastChecksum());

    // ���� accept �ɹ�����ʧ�ܣ����Ƕ���ʼ�µ�һ�ּ�����
    m_oMsgCounter.StartNewRound();

    AddAcceptTimer();

    PLGHead("END");

    // 3.27 : ���͸����еĽڵ㳢�� accept ���Լ�����ٳ��ԣ�������ʲô����ô?
    BroadcastMessage(oPaxosMsg, BroadcastMessage_Type_RunSelf_Final);
}

void Proposer :: OnAcceptReply(const PaxosMsg & oPaxosMsg)
{
    PLGHead("START Msg.ProposalID %lu State.ProposalID %lu Msg.from_nodeid %lu RejectByPromiseID %lu",
            oPaxosMsg.proposalid(), m_oProposerState.GetProposalID(), 
            oPaxosMsg.nodeid(), oPaxosMsg.rejectbypromiseid());

    BP->GetProposerBP()->OnAcceptReply();

    // �� onprepare һ����������Ϣ����ͬ��
    if (!m_bIsAccepting)
    {
        //PLGErr("Not proposing, skip this msg");
        BP->GetProposerBP()->OnAcceptReplyButNotAccepting();
        return;
    }

    if (oPaxosMsg.proposalid() != m_oProposerState.GetProposalID())
    {
        //PLGErr("ProposalID not same, skip this msg");
        BP->GetProposerBP()->OnAcceptReplyNotSameProposalIDMsg();
        return;
    }

    m_oMsgCounter.AddReceive(oPaxosMsg.nodeid());

    if (oPaxosMsg.rejectbypromiseid() == 0)
    {
        PLGDebug("[Accept]");
        m_oMsgCounter.AddPromiseOrAccept(oPaxosMsg.nodeid());
    }
    else
    {
        PLGDebug("[Reject]");
        m_oMsgCounter.AddReject(oPaxosMsg.nodeid());

        m_bWasRejectBySomeone = true;

        m_oProposerState.SetOtherProposalID(oPaxosMsg.rejectbypromiseid());
    }

    if (m_oMsgCounter.IsPassedOnThisRound())
    {
        int iUseTimeMs = m_oTimeStat.Point();
        BP->GetProposerBP()->AcceptPass(iUseTimeMs);
        PLGImp("[Pass] Start send learn, usetime %dms", iUseTimeMs);
        ExitAccept();
        m_poLearner->ProposerSendSuccess(GetInstanceID(), m_oProposerState.GetProposalID());
    }
    else if (m_oMsgCounter.IsRejectedOnThisRound()
            || m_oMsgCounter.IsAllReceiveOnThisRound())
    {
        BP->GetProposerBP()->AcceptNotPass();
        PLGImp("[Not pass] wait 30ms and Restart prepare");
        AddAcceptTimer(OtherUtils::FastRand() % 30 + 10);
    }

    PLGHead("END");
}

void Proposer :: OnExpiredAcceptReply(const PaxosMsg & oPaxosMsg)
{
    if (oPaxosMsg.rejectbypromiseid() != 0)
    {
        PLGDebug("[Expired Accept Reply Reject] RejectByPromiseID %lu", oPaxosMsg.rejectbypromiseid());
        m_bWasRejectBySomeone = true;
        m_oProposerState.SetOtherProposalID(oPaxosMsg.rejectbypromiseid());
    }
}

void Proposer :: OnPrepareTimeout()
{
    PLGHead("OK");

    if (GetInstanceID() != m_llTimeoutInstanceID)
    {
        PLGErr("TimeoutInstanceID %lu not same to NowInstanceID %lu, skip",
                m_llTimeoutInstanceID, GetInstanceID());
        return;
    }

    BP->GetProposerBP()->PrepareTimeout();

    // �������Ϊ�������ڵ㷢����ͻ�����·��� proposalID ֵ��
    Prepare(m_bWasRejectBySomeone);
}

void Proposer :: OnAcceptTimeout()
{
    PLGHead("OK");
    
    if (GetInstanceID() != m_llTimeoutInstanceID)
    {
        PLGErr("TimeoutInstanceID %lu not same to NowInstanceID %lu, skip",
                m_llTimeoutInstanceID, GetInstanceID());
        return;
    }
    
    BP->GetProposerBP()->AcceptTimeout();

    // ͬ��
    Prepare(m_bWasRejectBySomeone);
}

void Proposer :: CancelSkipPrepare()
{
    m_bCanSkipPrepare = false;
}

}


