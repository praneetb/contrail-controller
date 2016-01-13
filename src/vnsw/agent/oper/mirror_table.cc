/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/bind.hpp>
#include <base/logging.h>
#include <db/db.h>
#include <db/db_entry.h>
#include <db/db_table.h>

#include <cmn/agent_cmn.h>
#include "oper/route_common.h"
#include "oper/nexthop.h"
#include "oper/tunnel_nh.h"
#include "oper/vrf.h"
#include "oper/mirror_table.h"
#include "oper/agent_sandesh.h"

using namespace std;
using namespace boost::asio;

MirrorTable *MirrorTable::mirror_table_;

MirrorTable::~MirrorTable() { 
    boost::system::error_code err;
    if (udp_sock_.get()) {
        udp_sock_->close(err);
    }
}

bool MirrorEntry::IsLess(const DBEntry &rhs) const {
    const MirrorEntry &a = static_cast<const MirrorEntry &>(rhs);
    return (analyzer_name_ < a.GetAnalyzerName());
}

DBEntryBase::KeyPtr MirrorEntry::GetDBRequestKey() const {
    MirrorEntryKey *key = new MirrorEntryKey(analyzer_name_);
    return DBEntryBase::KeyPtr(key);
}

void MirrorEntry::SetKey(const DBRequestKey *k) {
    const MirrorEntryKey *key = static_cast<const MirrorEntryKey *>(k);
    analyzer_name_ = key->analyzer_name_;
}

std::auto_ptr<DBEntry> MirrorTable::AllocEntry(const DBRequestKey *k) const {
    const MirrorEntryKey *key = static_cast<const MirrorEntryKey *>(k);
    MirrorEntry *mirror_entry = new MirrorEntry(key->analyzer_name_);
    return std::auto_ptr<DBEntry>(static_cast<DBEntry *>(mirror_entry));
}

DBEntry *MirrorTable::Add(const DBRequest *req) {
    const MirrorEntryKey *key = static_cast<const MirrorEntryKey *>(req->key.get());
    MirrorEntry *mirror_entry = new MirrorEntry(key->analyzer_name_);
    //Get Mirror NH
    OnChange(mirror_entry, req);
    return mirror_entry;
}

bool MirrorTable::OnChange(DBEntry *entry, const DBRequest *req) {
    bool ret = false; 
    MirrorEntry *mirror_entry = static_cast<MirrorEntry *>(entry);
    MirrorEntryData *data = static_cast<MirrorEntryData *>(req->data.get());

    mirror_entry->vrf_name_ = data->vrf_name_;
    mirror_entry->sip_ = data->sip_;
    mirror_entry->sport_ = data->sport_;
    mirror_entry->dip_ = data->dip_;
    mirror_entry->dport_ = data->dport_;

    DBRequest nh_req;
    nh_req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    MirrorNHKey *nh_key = new MirrorNHKey(data->vrf_name_, data->sip_,
                                  data->sport_, data->dip_, data->dport_);
    nh_req.key.reset(nh_key);
    nh_req.data.reset(NULL);
    agent()->nexthop_table()->Process(nh_req);

    VrfKey key(data->vrf_name_);
    VrfEntry *vrf =
        static_cast<VrfEntry *>(agent()->vrf_table()->FindActiveEntry(&key));

    NextHop *nh = static_cast<NextHop *>
                  (agent()->nexthop_table()->FindActiveEntry(nh_key));
    if (nh == NULL || vrf == NULL) {
        //Make the mirror NH point to discard
        //and change the nexthop once the VRF is
        //available
        AddUnresolved(mirror_entry);
        DiscardNH key;
        nh = static_cast<NextHop *>
            (agent()->nexthop_table()->FindActiveEntry(&key));
    } else {
        AddResolvedVrfMirrorEntry(mirror_entry);
    }

    if (mirror_entry->nh_ != nh) {
        mirror_entry->nh_ = nh;
        mirror_entry->vrf_ =
            agent()->vrf_table()->FindVrfFromName(data->vrf_name_);
        ret = true;
    }
    return ret;
}

bool MirrorTable::Delete(DBEntry *entry, const DBRequest *request) {
    MirrorEntry *mirror_entry = static_cast<MirrorEntry *>(entry);
    RemoveUnresolved(mirror_entry);
    DeleteResolvedVrfMirrorEntry(mirror_entry);
    return true;
}

void MirrorTable::Add(VrfMirrorEntryList &vrf_entry_map, MirrorEntry *entry) {
    VrfMirrorEntryList::iterator it = vrf_entry_map.find(entry->vrf_name_);

    if (it != vrf_entry_map.end()) {
        MirrorEntryList::const_iterator list_it = it->second.begin();
        for (; list_it != it->second.end(); list_it++) {
            if (*list_it == entry) {
                //Entry already present
                return;
            }
        }
        it->second.push_back(entry);
        return;
    }

    MirrorEntryList list;
    list.push_back(entry);
    vrf_entry_map.insert(VrfMirrorEntry(entry->vrf_name_, list));
}

void MirrorTable::Delete(VrfMirrorEntryList &list, MirrorEntry *entry) {
    VrfMirrorEntryList::iterator it = list.find(entry->vrf_name_);
    if (it == list.end()) {
        return;
    }

    MirrorEntryList::iterator list_it = it->second.begin();
    for(;list_it != it->second.end(); list_it++) {
        if (*list_it == entry) {
            it->second.erase(list_it);
            break;
        }
    }
}

void MirrorTable::ResyncMirrorEntry(VrfMirrorEntryList &list,
                                    const VrfEntry *vrf) {
    VrfMirrorEntryList::iterator it = list.find(vrf->GetName());
    if (it == list.end()) {
        return;
    }

    MirrorEntryList::iterator list_it = it->second.begin();
    for(;list_it != it->second.end(); list_it++) {
        DBRequest req;
        req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

        MirrorEntryKey *key = new MirrorEntryKey((*list_it)->GetAnalyzerName());
        key->sub_op_ = AgentKey::RESYNC;
        MirrorEntryData *data = new MirrorEntryData((*list_it)->vrf_name(),
                *((*list_it)->GetSip()),
                (*list_it)->GetSPort(),
                *((*list_it)->GetDip()),
                (*list_it)->GetDPort());
        req.key.reset(key);
        req.data.reset(data);
        Enqueue(&req);
    }
    list.erase(it);
}

void MirrorTable::AddUnresolved(MirrorEntry *entry) {
    Add(unresolved_entry_list_, entry);
}

void MirrorTable::RemoveUnresolved(MirrorEntry *entry) {
    Delete(unresolved_entry_list_, entry);
}

void MirrorTable::AddResolvedVrfMirrorEntry(MirrorEntry *entry) {
    Add(resolved_entry_list_, entry);
}

void MirrorTable::DeleteResolvedVrfMirrorEntry(MirrorEntry *entry) {
    Delete(resolved_entry_list_, entry);
}

void MirrorTable::ResyncResolvedMirrorEntry(const VrfEntry *vrf) {
    ResyncMirrorEntry(resolved_entry_list_, vrf);
}

void MirrorTable::ResyncUnresolvedMirrorEntry(const VrfEntry *vrf) {
    ResyncMirrorEntry(unresolved_entry_list_, vrf);
}

void MirrorTable::AddMirrorEntry(const std::string &analyzer_name,
                                 const std::string &vrf_name,
                                 const Ip4Address &sip, uint16_t sport, 
                                 const Ip4Address &dip, uint16_t dport) {

    DBRequest req;
    // First enqueue request to add Mirror NH
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

    MirrorNHKey *nh_key = new MirrorNHKey(vrf_name, sip, sport, dip, dport);
    req.key.reset(nh_key);
    req.data.reset(NULL);
    mirror_table_->agent()->nexthop_table()->Enqueue(&req);

    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    MirrorEntryKey *key = new MirrorEntryKey(analyzer_name);
    MirrorEntryData *data = new MirrorEntryData(vrf_name, sip, 
                                                sport, dip, dport);
    req.key.reset(key);
    req.data.reset(data);
    mirror_table_->Enqueue(&req);
}

void MirrorTable::DelMirrorEntry(const std::string &analyzer_name) {
    DBRequest req;
    req.oper = DBRequest::DB_ENTRY_DELETE;
    MirrorEntryKey *key = new MirrorEntryKey(analyzer_name);
    req.key.reset(key);
    req.data.reset(NULL);
    mirror_table_->Enqueue(&req);
}

void MirrorTable::OnZeroRefcount(AgentDBEntry *e) {
    const MirrorEntry *mirr_entry = static_cast<const MirrorEntry *>(e);
    DelMirrorEntry(mirr_entry->GetAnalyzerName());
}

DBTableBase *MirrorTable::CreateTable(DB *db, const std::string &name) {
    mirror_table_ = new MirrorTable(db, name);
    mirror_table_->Init();
    return mirror_table_;
};

void MirrorTable::Initialize() {
    VrfListenerInit();
}

void MirrorTable::VrfListenerInit() {
    vrf_listener_id_ = agent()->vrf_table()->
                           Register(boost::bind(&MirrorTable::VrfNotify,
                                    this, _1, _2));
}

void MirrorTable::VrfNotify(DBTablePartBase *base, DBEntryBase *entry) {
    const VrfEntry *vrf = static_cast<VrfEntry *>(entry);
    if (vrf->IsDeleted()) {
        //VRF is getting deleted remove all the mirror nexthop
        ResyncResolvedMirrorEntry(vrf);
        return;
    }

    ResyncUnresolvedMirrorEntry(vrf);
}

void MirrorTable::ReadHandler(const boost::system::error_code &ec,
                              size_t bytes_transferred) {

    if (ec) {
        LOG(ERROR, "Error reading from Mirroor sock. Error : " << 
            boost::system::system_error(ec).what());
        return;
    }

    udp_sock_->async_receive(boost::asio::buffer(rx_buff_, sizeof(rx_buff_)), 
                           boost::bind(&MirrorTable::ReadHandler, this, 
                                       placeholders::error,
                                       placeholders::bytes_transferred));
}

void MirrorTable::MirrorSockInit(void) {
    EventManager *event_mgr;

    event_mgr = agent()->event_manager();
    boost::asio::io_service &io = *event_mgr->io_service();
    ip::udp::endpoint ep(ip::udp::v4(), 0);

    udp_sock_.reset(new ip::udp::socket(io));

    boost::system::error_code ec;
    udp_sock_->open(ip::udp::v4(), ec);
    assert(ec.value() == 0);

    udp_sock_->bind(ep, ec);
    assert(ec.value() == 0);

    ip::udp::endpoint sock_ep = udp_sock_->local_endpoint(ec);
    assert(ec.value() == 0);
    agent()->set_mirror_port(sock_ep.port());

    udp_sock_->async_receive(boost::asio::buffer(rx_buff_, sizeof(rx_buff_)), 
                             boost::bind(&MirrorTable::ReadHandler, this, 
                                         placeholders::error,
                                         placeholders::bytes_transferred));
}

VrfEntry *MirrorTable::FindVrfEntry(const string &vrf_name) const {
    return agent()->vrf_table()->FindVrfFromName(vrf_name);
}

void MirrorTable::Shutdown() {
    agent()->vrf_table()->Unregister(vrf_listener_id_);
}

uint32_t MirrorEntry::vrf_id() const {
    return vrf_ ? vrf_->vrf_id() : uint32_t(-1);
}

const VrfEntry *MirrorEntry::GetVrf() const {
    return vrf_ ? vrf_.get() : NULL;
}

void MirrorEntry::set_mirror_entrySandeshData(MirrorEntrySandeshData &data) const {
    data.set_analyzer_name(GetAnalyzerName());
    data.set_sip(GetSip()->to_string());
    data.set_dip(GetDip()->to_string());
    data.set_vrf(GetVrf() ? GetVrf()->GetName() : "");
    data.set_sport(GetSPort());
    data.set_dport(GetDPort());
    data.set_ref_count(GetRefCount());
    nh_->SetNHSandeshData(data.nh);
}

bool MirrorEntry::DBEntrySandesh(Sandesh *sresp, std::string &name) const {
    MirrorEntryResp *resp = static_cast<MirrorEntryResp *>(sresp);

    MirrorEntrySandeshData data;
    set_mirror_entrySandeshData(data);
    std::vector<MirrorEntrySandeshData> &list =  
        const_cast<std::vector<MirrorEntrySandeshData>&>
        (resp->get_mirror_entry_list());
    list.push_back(data);

    return true;
}

void MirrorEntryReq::HandleRequest() const {
    AgentSandeshPtr sand(new AgentMirrorSandesh(context(), get_analyzer_name()));
    sand->DoSandesh(sand);
}

AgentSandeshPtr MirrorTable::GetAgentSandesh(const AgentSandeshArguments *args,
                                             const std::string &context) {
    return AgentSandeshPtr(new AgentMirrorSandesh(context,
                                            args->GetString("analyzer_name")));
}
