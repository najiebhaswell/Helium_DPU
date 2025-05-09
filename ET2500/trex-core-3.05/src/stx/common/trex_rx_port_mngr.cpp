/*
  Itay Marom
  Cisco Systems, Inc.
*/

/*
  Copyright (c) 2016-2016 Cisco Systems, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "bp_sim.h"
#include "trex_rx_port_mngr.h"
#include "trex_rx_core.h"
#include "pkt_gen.h"
#include "trex_capture.h"
#include "utl_mbuf.h"
#include <zmq.h>



RxAstfLatency::RxAstfLatency(){
    m_rx_core=0;
    m_port_id=255;
}

void RxAstfLatency::create(CRxCore *rx_core,
                           uint8_t port_id){
    m_rx_core = rx_core;
    m_port_id = port_id;
}

void RxAstfLatency::handle_pkt(const rte_mbuf_t *m){
    m_rx_core->handle_astf_latency_pkt(m, m_port_id);
}


/**************************************
 * RX feature queue 
 * 
 *************************************/

void
RXQueue::start(uint64_t size) {
    if (m_pkt_buffer) {
        delete m_pkt_buffer;
    }
    m_pkt_buffer = new TrexPktBuffer(size, TrexPktBuffer::MODE_DROP_HEAD);
}

void
RXQueue::stop() {
    if (m_pkt_buffer) {
        delete m_pkt_buffer;
        m_pkt_buffer = NULL;
    }
}

const TrexPktBuffer *
RXQueue::fetch() {

    /* if no buffer or the buffer is empty - give a NULL one */
    if ( (!m_pkt_buffer) || (m_pkt_buffer->get_element_count() == 0) ) {
        return nullptr;
    }
    
    /* hold a pointer to the old one */
    TrexPktBuffer *old_buffer = m_pkt_buffer;

    /* replace the old one with a new one and freeze the old */
    m_pkt_buffer = new TrexPktBuffer(old_buffer->get_capacity(), old_buffer->get_mode());

    return old_buffer;
}

void
RXQueue::handle_pkt(const rte_mbuf_t *m) {
    m_pkt_buffer->push(m);
}

Json::Value
RXQueue::to_json() const {
    assert(m_pkt_buffer != NULL);
    
    Json::Value output = Json::objectValue;
    
    output["size"]    = Json::UInt64(m_pkt_buffer->get_capacity());
    output["count"]   = Json::UInt64(m_pkt_buffer->get_element_count());
    
    return output;
}


/**************************************
 * Capture Port
 *
 *************************************/
void
RXCapturePort::create(RXFeatureAPI *api) {
    m_api = api;
}

Json::Value
RXCapturePort::to_json() const {
    Json::Value output = Json::objectValue;
    output["bpf_filter"] = m_bpf_filter ? m_bpf_filter->get_filter() : "";
    output["endpoint"] = m_endpoint;
    output["socket_open"] = m_zeromq_socket != nullptr;
    return output;
}


void
RXCapturePort::handle_pkt(const rte_mbuf_t *m) {
    if(likely(m_bpf_filter != nullptr)) {
        if (!m_bpf_filter->match(m)) {
            return;
        }
    }

    uint8_t* ptr = rte_pktmbuf_mtod(m, uint8_t *);
    uint32_t len = rte_pktmbuf_pkt_len(m);
    zmq_send(m_zeromq_socket, ptr, len, ZMQ_DONTWAIT);
}

void
RXCapturePort::set_bpf_filter(const std::string& filter) {
   delete m_bpf_filter;
   m_bpf_filter = nullptr;
   if (filter.size() > 0) {
       m_bpf_filter = new BPFFilter();
       m_bpf_filter->set_filter(filter);
       m_bpf_filter->compile();
   }
}

bool RXCapturePort::start(std::string &err) {
    m_zeromq_ctx = zmq_ctx_new();
    if ( !m_zeromq_ctx ) {
        err = "Could not create ZMQ context";
        return false;
    }
    m_zeromq_socket = zmq_socket(m_zeromq_ctx, ZMQ_PAIR);
    if ( !m_zeromq_socket ) {
        zmq_ctx_term(m_zeromq_ctx);
        m_zeromq_ctx = nullptr;
        err = "Could not create ZMQ socket";
        return false;
    }
    int linger = 0;
    zmq_setsockopt(m_zeromq_socket, ZMQ_LINGER, &linger, sizeof(linger));
    if ( zmq_connect(m_zeromq_socket, m_endpoint.c_str()) != 0 ) {
        zmq_close(m_zeromq_socket);
        zmq_ctx_term(m_zeromq_ctx);
        m_zeromq_socket = nullptr;
        m_zeromq_ctx = nullptr;
        err = "Could not connect to ZMQ socket";
        return false;
    }
    return true;
}

void RXCapturePort::stop() {
    if ( m_zeromq_socket ) {
        zmq_close(m_zeromq_socket);
        m_zeromq_socket = nullptr;
    }
    if ( m_zeromq_ctx ) {
        zmq_ctx_term(m_zeromq_ctx);
        m_zeromq_ctx = nullptr;
    }
}

uint32_t
RXCapturePort::do_tx() {
    uint32_t cnt = 0;
    uint8_t port_id = m_api->get_port_id();
    int len = 0;
    while(cnt < 32 && (len = zmq_recv(m_zeromq_socket, m_buffer_zmq,
                               sizeof(m_buffer_zmq), ZMQ_NOBLOCK)) > 0) {
        /* Allocate a mbuf */
        rte_mbuf_t *m = CGlobalInfo::pktmbuf_alloc(CGlobalInfo::m_socket.port_to_socket(port_id), len);
        assert(m);

        /* allocate */
        uint8_t *p = (uint8_t *)rte_pktmbuf_append(m, len);
        assert(p);

        memcpy(p, m_buffer_zmq, len);
        if (!m_api->tx_pkt(m)) {
            rte_pktmbuf_free(m);
            break;
        }

        cnt++;
    }

    return cnt;
}

RXCapturePort::~RXCapturePort() {
    delete m_bpf_filter;
    stop();
    m_bpf_filter = nullptr;
}


/**************************************
 * CAPWAP proxy
 * 
 *************************************/

#ifndef WLAN_IP_OFFSET
#define WLAN_IP_OFFSET 84
#endif


void RXCapwapProxy::create(RXFeatureAPI *api) {
    m_api = api;
#if 0 //not support by marvin
    m_wired_bpf_filter = bpfjit_compile("ip and udp src port 5247 and udp[48:2] == 2048");
#endif
}


void
RXCapwapProxy::reset() {
    // clear counters
    m_bpf_rejected = 0;
    m_ip_convert_err = 0;
    m_map_not_found = 0;
    m_not_ip = 0;
    m_too_large_pkt = 0;
    m_too_small_pkt = 0;
    m_tx_err = 0;
    m_tx_ok = 0;
    m_pkt_from_wlc = 0;

    m_capwap_map.clear();
}


bool
RXCapwapProxy::set_values(uint8_t pair_port_id, bool is_wireless_side, Json::Value capwap_map, uint32_t wlc_ip) {
    m_is_wireless_side = is_wireless_side;
    m_pair_port_id = pair_port_id;
    m_wlc_ip = wlc_ip;
    reset();
    std::string wrap_data;

    for (const std::string &client_ip_str : capwap_map.getMemberNames()) {
        wrap_data = base64_decode(capwap_map[client_ip_str].asString());
        rc = utl_ipv4_to_uint32(client_ip_str.c_str(), m_client_ip_num);
        if ( !rc ) {
            m_ip_convert_err += 1;
            return false;
        }
        m_capwap_map[m_client_ip_num] = wrap_data;
    }
    return true;
}

bool
RXCapwapProxy::add_values(Json::Value capwap_map) {
    std::string wrap_data;

    for (const std::string &client_ip_str : capwap_map.getMemberNames()) {
        wrap_data = base64_decode(capwap_map[client_ip_str].asString());
        rc = utl_ipv4_to_uint32(client_ip_str.c_str(), m_client_ip_num);
        if ( !rc ) {
            m_ip_convert_err += 1;
            return false;
        }
        m_capwap_map[m_client_ip_num] = wrap_data;
    }
    return true;
}


Json::Value
RXCapwapProxy::to_json() const {
    Json::Value output = Json::objectValue;
    std::string client_ip, encoded_pkt;

    Json::Value capwap_map_json = Json::objectValue;
    for (auto &x: m_capwap_map) {
        client_ip = utl_uint32_to_ipv4(x.first);
        encoded_pkt = base64_encode((unsigned char *) x.second.c_str(), x.second.size());
        capwap_map_json[client_ip] = encoded_pkt;
    }
    output["capwap_map"]        = capwap_map_json;
    output["is_wireless_side"]  = m_is_wireless_side;
    output["pair_port_id"]      = m_pair_port_id;
    output["wlc_ip"]            = m_wlc_ip;

    // counters
    Json::Value counters   = Json::objectValue;
    counters["m_bpf_rejected"]          = Json::UInt64(m_bpf_rejected);
    counters["m_ip_convert_err"]        = Json::UInt64(m_ip_convert_err);
    counters["m_map_not_found"]         = Json::UInt64(m_map_not_found);
    counters["m_not_ip"]                = Json::UInt64(m_not_ip);
    counters["m_too_large_pkt"]         = Json::UInt64(m_too_large_pkt);
    counters["m_too_small_pkt"]         = Json::UInt64(m_too_small_pkt);
    counters["m_tx_err"]                = Json::UInt64(m_tx_err);
    counters["m_tx_ok"]                 = Json::UInt64(m_tx_ok);
    counters["m_pkt_from_wlc"]          = Json::UInt64(m_pkt_from_wlc);
    output["counters"] = counters;

    return output;
}


// send to pair port after stripping or adding CAPWAP info
rx_pkt_action_t
RXCapwapProxy::handle_pkt(rte_mbuf_t *m) {
    if ( m_is_wireless_side ) {
        return handle_wireless(m);
    } else {
        return handle_wired(m);
    }
}


/*
No checks of AP and client MAC!
*/
rx_pkt_action_t
RXCapwapProxy::handle_wired(rte_mbuf_t *m) {
    uint16_t rx_pkt_size = rte_pktmbuf_pkt_len(m);
    if ( unlikely(rx_pkt_size < WLAN_IP_OFFSET + ETH_HDR_LEN + IPV4_HDR_LEN) ) { // not accurate but sufficient
        m_too_small_pkt += 1;
        return RX_PKT_FREE;
    }
    m_pkt_data_ptr = rte_pktmbuf_mtod(m, char *);

    rc = bpfjit_run(m_wired_bpf_filter, m_pkt_data_ptr, rx_pkt_size);
    if ( unlikely(!rc) ) {
        m_bpf_rejected += 1;
        return RX_PKT_FREE;
    }

    m_ipv4 = (IPHeader *)(m_pkt_data_ptr + WLAN_IP_OFFSET);

    // get dst IP
    m_client_ip_num = m_ipv4->getDestIp();
    m_capwap_map_it = m_capwap_map.find(m_client_ip_num);
    if ( unlikely(m_capwap_map_it == m_capwap_map.end()) ) {
        m_map_not_found += 1;
        return RX_PKT_FREE;
    }

    // get src IP
    uint32_t src_ip = m_ipv4->getSourceIp();
    if (unlikely( src_ip == m_wlc_ip )) {
        m_pkt_from_wlc += 1;
        return RX_PKT_FREE;
    }

    // removing capwap+wlan and adding ether
    rte_pktmbuf_adj(m, (WLAN_IP_OFFSET - ETH_HDR_LEN));
    memcpy(m_pkt_data_ptr + WLAN_IP_OFFSET - ETH_HDR_LEN, m_capwap_map_it->second.c_str(), m_capwap_map_it->second.size());

    rc = m_api->tx_pkt(m, m_pair_port_id);
    if ( unlikely(!rc) ) {
        m_tx_err += 1;
        return RX_PKT_FREE;
    }
    m_tx_ok += 1;
    return RX_PKT_NOOP;
}


/*
No checks of client MAC!
*/
rx_pkt_action_t
RXCapwapProxy::handle_wireless(rte_mbuf_t *m) {
    uint16_t rx_pkt_size = rte_pktmbuf_pkt_len(m);
    if ( unlikely(rx_pkt_size < ETH_HDR_LEN + IPV4_HDR_LEN) ) {
        m_too_small_pkt += 1;
        return RX_PKT_FREE;
    }
    m_pkt_data_ptr = rte_pktmbuf_mtod(m, char *);

    // verify IP layer
    m_ether = (EthernetHeader *)m_pkt_data_ptr;
    if ( unlikely(m_ether->getNextProtocol() != EthernetHeader::Protocol::IP) ) {
        m_not_ip += 1;
        return RX_PKT_FREE;
    }

    // get src IP
    m_ipv4 = (IPHeader *)(m_pkt_data_ptr + ETH_HDR_LEN);
    m_client_ip_num = m_ipv4->getSourceIp();
    m_capwap_map_it = m_capwap_map.find(m_client_ip_num);
    if ( unlikely(m_capwap_map_it == m_capwap_map.end()) ) {
        m_map_not_found += 1;
        return RX_PKT_FREE;
    }

    if ( unlikely(rx_pkt_size > MAX_PKT_ALIGN_BUF_9K - (m_capwap_map_it->second.size() - ETH_HDR_LEN)) ) {
        m_too_large_pkt += 1;
        return RX_PKT_FREE;
    }

    m_new_ip_length = rx_pkt_size + m_capwap_map_it->second.size() - ETH_HDR_LEN - ETH_HDR_LEN; //adding capwap+wlan and removing ether

    // Fix IP total length and checksum
    m_ipv4 = (IPHeader *)(m_capwap_map_it->second.c_str() + ETH_HDR_LEN);
    m_ipv4->updateTotalLength(m_new_ip_length);

    // Update UDP length
    UDPHeader *udp = (UDPHeader *)(m_capwap_map_it->second.c_str() + ETH_HDR_LEN + IPV4_HDR_LEN);
    udp->setLength(m_new_ip_length - IPV4_HDR_LEN);

    // allocate new mbuf and chain it to received one
    m_mbuf_ptr = CGlobalInfo::pktmbuf_alloc_by_port(m_pair_port_id, m_capwap_map_it->second.size());
    memcpy(rte_pktmbuf_mtod(m_mbuf_ptr, char *), m_capwap_map_it->second.c_str(), m_capwap_map_it->second.size());

    rte_pktmbuf_adj(m, ETH_HDR_LEN);
    m_mbuf_ptr->next = m;
    m_mbuf_ptr->data_len = m_capwap_map_it->second.size();
    m_mbuf_ptr->pkt_len = m_mbuf_ptr->data_len + m->pkt_len;
    m_mbuf_ptr->nb_segs = m->nb_segs + 1;
    m->pkt_len = m->data_len;

    rc = m_api->tx_pkt(m_mbuf_ptr, m_pair_port_id);
    if ( unlikely(!rc) ) {
        m_tx_err += 1;
        rte_pktmbuf_free(m_mbuf_ptr);
        return RX_PKT_NOOP;
    }
    m_tx_ok += 1;
    return RX_PKT_NOOP;
}


/**************************************
 * Port manager 
 * 
 *************************************/

RXPortManager::RXPortManager() : m_feature_api(this) {
    clear_all_features();
    m_stack          = nullptr;
    m_io             = nullptr;
    m_tpg            = nullptr;
    m_port_id        = UINT8_MAX;
}

RXPortManager::~RXPortManager(void) {
    delete m_stack;
    delete m_parser;
    delete m_tpg;
}

struct CPlatformYamlInfo;

void
RXPortManager::create_async(uint32_t port_id,
                      CRxCore *rx_core,
                      CPortLatencyHWBase *io,
                      CRFC2544Info *rfc2544,
                      CRxCoreErrCntrs *err_cntrs,
                      CCpuUtlDp *cpu_util) {
    
    m_port_id = port_id;
    m_io = io;
    m_rx_core = rx_core;

    /* create a predicator for CPU util. */
    m_cpu_pred.create(cpu_util);
    
    /* init features */
    m_latency.create(rfc2544, err_cntrs);
    m_astf_latency.create(m_rx_core,port_id);

    m_capwap_proxy.create(&m_feature_api);
    m_capture_port.create(&m_feature_api);

    /* by default, stack is always on */
    std::string &stack_type = CGlobalInfo::m_options.m_stack_type;
    m_stack = CStackFactory::create(stack_type, &m_feature_api, &m_ign_stats);
    m_stack->add_port_node_async();
    m_stack->run_pending_tasks_async(0,false);
    m_parser = new CFlowStatParser(CFlowStatParser::FLOW_STAT_PARSER_MODE_SW);

    set_feature(STACK);
    m_tunnel_handler = nullptr;
}

std::string wait_stack_tasks(CStackBase *stack, double timeout_sec) {
    stack_result_t results;
    try {
        stack->wait_on_tasks(0, results, timeout_sec);
    } catch (const TrexException &ex) {
        return ex.what();
    }
    if ( results.err_per_mac.size() ) {
        std::string error;

        if ( results.err_per_mac.size() == 1 ) {
            error = results.err_per_mac.begin()->second;
        } else {
            stringstream ss;
            ss << "Got multiple errors: " << std::endl;
            for (auto &error : results.err_per_mac ) {
                ss << "* " <<  error.second << endl;
            }
            error = ss.str();
        }
        std::cout << error << std::endl;
        return error;
    }
    return "";
}

void RXPortManager::wait_for_create_done(void) {
    std::string err = wait_stack_tasks(get_stack(), 5);
    if ( err.size() ) {
        throw TrexException("Error: cound not create stack with following reason:\n" + err);
    }
}

void RXPortManager::cleanup_async(void) {
    m_stack->cleanup_async();
    m_stack->run_pending_tasks_async(0,false);
}

void RXPortManager::wait_for_cleanup_done(void) {
    wait_stack_tasks(get_stack(), -1);
}

bool RXPortManager::start_capture_port(const std::string& filter,
                                       const std::string& endpoint,
                                       std::string &err) {
    if ( is_feature_set(CAPTURE_PORT) ) {
        err = "Capture port is already active";
        return false;
    }

    /* Set the BPF filter */
    m_capture_port.set_bpf_filter(filter);

    /* Set the endpoint */
    m_capture_port.set_endpoint(endpoint);

    /* Start the zeromq socket */
    if ( !m_capture_port.start(err) ) {
        return false;
    }

    set_feature(CAPTURE_PORT);
    return true;
}

bool RXPortManager::stop_capture_port(std::string &err) {

    if ( !is_feature_set(CAPTURE_PORT) ) {
        return true;
        /* allow stopping stopped
        err = "Capture port is not active";
        return false;
        */
    }

    m_capture_port.stop();
    unset_feature(CAPTURE_PORT);
    return true;
}

void RXPortManager::enable_tpg(uint32_t num_tpgids, PacketGroupTagMgr* tag_mgr, CTPGTagCntr* port_cntrs) {
    set_feature(TPG);
    assert(m_tpg == nullptr);
    m_tpg = new RxTPGPerPort(m_port_id, num_tpgids, tag_mgr, port_cntrs);
}

void RXPortManager::disable_tpg() {
    unset_feature(TPG);
    delete m_tpg;
    m_tpg = nullptr;
}

void RXPortManager::handle_pkt(rte_mbuf_t *m) {

    /* handle features */

    /* capture should be first */
    TrexCaptureMngr::getInstance().handle_pkt_rx(m, m_port_id);

    if (is_feature_set(LATENCY)) {
        m_latency.handle_pkt(m, m_port_id);
    }

    if (is_feature_set(TPG)) {
        m_tpg->handle_pkt(m);
    }

    if (is_feature_set(QUEUE)) {
        m_queue.handle_pkt(m);
    }


    if ( is_feature_set(STACK) /*&& !m_stack->is_running_tasks()*/ ) {
        m_stack->handle_pkt(m);
    }


    if ( is_feature_set(EZMQ) ){
        if (is_emu_filter(m) ) {
            m_zmq_wr->write_pkt(m,m_port_id);
        }
    }

    if (is_feature_set(CAPTURE_PORT)) {
        m_capture_port.handle_pkt(m);
    }

    if (is_feature_set(ASTF_LATENCY)) { 
        m_astf_latency.handle_pkt(m);
    }

    if (is_feature_set(CAPWAP_PROXY)) { // changes the mbuf, so need to be last.
        rx_pkt_action_t action;
        action = m_capwap_proxy.handle_pkt((rte_mbuf_t *) m);
        if ( action == RX_PKT_NOOP ) {
            return;
        }
    }

    rte_pktmbuf_free(m);
}

/* quick filter to verify that the packet is eligible for emu. in case of multi-core the filter is done in 
DP cores and this is just a quick filter */
bool RXPortManager::is_emu_filter(rte_mbuf_t *m){
    uint8_t *p = rte_pktmbuf_mtod(m, uint8_t*);
    uint16_t pkt_size= rte_pktmbuf_data_len(m);
    CFlowStatParser_err_t res=m_parser->parse(p,pkt_size);
    if (res != FSTAT_PARSER_E_OK){
        return true;
    }
    uint8_t proto = m_parser->get_protocol();
    bool tcp_udp=false;

    if ((proto == IPPROTO_TCP) || (proto == IPPROTO_UDP)){
        tcp_udp = true;
    }
    if (!tcp_udp){
        return true;
    }

    if (proto == IPPROTO_UDP) {
        UDPHeader *l4_header = (UDPHeader *)m_parser->get_l4();
        uint16_t src_port = l4_header->getSourcePort();
        uint16_t dst_port = l4_header->getDestPort();
        if ( (( src_port == DHCPv4_PORT || dst_port == DHCPv4_PORT ))  ||
             (( src_port == DHCPv6_PORT || dst_port == DHCPv6_PORT ))) {
            return true;
        }
        if (dst_port == MDNS_PORT && src_port == MDNS_PORT) {
            // In MDNS both ports should equal.
            return true;
        }
    }

    if  ( (proto == IPPROTO_UDP) || (proto == IPPROTO_TCP) ) {
        UDPHeader *l4_header = (UDPHeader *)m_parser->get_l4();
        uint16_t src_port = l4_header->getSourcePort();
        uint16_t dst_port = l4_header->getDestPort();
        if ( (dst_port & 0xff00) == 0xff00 || (src_port & 0xff00) == 0xff00 ) {
            return true;
        }
    }

    return false;
}

uint16_t RXPortManager::handle_tx(void) {
    uint16_t cnt_pkts = 0;
    uint16_t limit = 32;

    if ( is_feature_set(STACK) && !m_stack->is_running_tasks() ) {
        cnt_pkts += m_stack->handle_tx(limit);
    }

    /* Do TX on capture port if needed */
    if ( is_feature_set(CAPTURE_PORT) ) {
        cnt_pkts += m_capture_port.do_tx();
    }

    return cnt_pkts;
}

int RXPortManager::process_all_pending_pkts(bool flush_rx) {

    rte_mbuf_t *rx_pkts[64];

    /* start CPU util. with heuristics
       this may or may not start the CPU util.
       measurement
     */
    m_cpu_pred.start_heur();
    uint16_t cnt_tx = handle_tx();

    if ( cnt_tx ) {
        m_cpu_pred.update(true);
    }

    /* try to read 64 packets clean up the queue */
    uint16_t cnt_rx = m_io->rx_burst(rx_pkts, 64);

    if (cnt_tx + cnt_rx == 0) {
        /* update the predicator that no packets have arrived
           will stop the predictor if it was started
         */
        m_cpu_pred.update(false);
        return 0;
    }

    /* for next time mark the predictor as true
       will start the predictor in case it did not
     */
    m_cpu_pred.update(true);

    for (int j = 0; j < cnt_rx; j++) {
        rte_mbuf_t *m = rx_pkts[j];

        if (!flush_rx) {
            handle_pkt(m);
        } else {
            rte_pktmbuf_free(m);
        }
    }

    /* done */
    m_cpu_pred.commit();
    

    return cnt_tx + cnt_rx;
}


bool
RXPortManager::tx_pkt(const std::string &pkt) {
    /* allocate MBUF */
    rte_mbuf_t *m;
    socket_id_t socket = CGlobalInfo::m_socket.port_to_socket(m_port_id);
    
    if ( likely(pkt.size() <= _2048_MBUF_SIZE) ) {
        m = CGlobalInfo::pktmbuf_alloc_no_assert(socket, pkt.size());
        if ( m ) {
            /* allocate */
            uint8_t *p = (uint8_t *)rte_pktmbuf_append(m, pkt.size());
            assert(p);
            /* copy */
            memcpy(p, pkt.c_str(), pkt.size());
        }
    } else {
        /*  creating chaning of mbuf in the size of pool */
        rte_mempool *pool_2k = CGlobalInfo::pktmbuf_get_pool(socket, _2048_MBUF_SIZE);
        m = utl_rte_pktmbuf_mem_to_pkt_no_assert(pkt.c_str(), pkt.size(), _2048_MBUF_SIZE, pool_2k);
    }

    if ( !m ) {
        m_stack->get_counters().m_tx_dropped_no_mbuf++;
        return false;
    }
    /* send */
    bool rc = tx_pkt(m);
    if (!rc) {
        rte_pktmbuf_free(m);
    }
    
    return rc;
}


bool
RXPortManager::tx_pkt(rte_mbuf_t *m) {
    //tunnel mode
    if (m_tunnel_handler) {
        pkt_dir_t dir = port_id_to_dir(m_port_id);
        if (m_tunnel_handler->on_tx(dir, m)) {
            return false;
        }
    }
    TrexCaptureMngr::getInstance().handle_pkt_tx(m, m_port_id);
    return (m_io->tx_raw(m) == 0);
}


void RXPortManager::to_json(Json::Value &feat_res) const {
    if (is_feature_set(LATENCY)) {
        feat_res["latency"] = m_latency.to_json();
        feat_res["latency"]["is_active"] = true;
    } else {
        feat_res["latency"]["is_active"] = false;
    }

    if (is_feature_set(QUEUE)) {
        feat_res["queue"] = m_queue.to_json();
        feat_res["queue"]["is_active"] = true;
    } else {
        feat_res["queue"]["is_active"] = false;
    }

    if (is_feature_set(STACK)) {
        feat_res["stack"]["is_active"] = true;
        m_stack->grat_to_json(feat_res["grat_arp"]);
    } else {
        feat_res["stack"]["is_active"] = false;
        feat_res["grat_arp"]["is_active"] = false;
    }

    if (is_feature_set(CAPTURE_PORT)) {
        feat_res["capture_port"] = m_capture_port.to_json();
        feat_res["capture_port"]["is_active"] = true;
    } else {
        feat_res["capture_port"]["is_active"] = false;
    }

    if (is_feature_set(CAPWAP_PROXY)) {
        feat_res["capwap_proxy"] = m_capwap_proxy.to_json();
        feat_res["capwap_proxy"]["is_active"] = true;
    } else {
        feat_res["capwap_proxy"]["is_active"] = false;
    }

}


void RXPortManager::get_ignore_stats(CRXCoreIgnoreStat &stat, bool get_diff) {
    if (get_diff) {
        stat = m_ign_stats - m_ign_stats_prev;
        m_ign_stats_prev = m_ign_stats;
    } else {
        stat = m_ign_stats;
    }
}


/**************************************
 * RX feature API
 * exposes a subset of commands 
 * from the port manager object 
 *************************************/

bool
RXFeatureAPI::tx_pkt(const std::string &pkt) {
    return m_port_mngr->tx_pkt(pkt);
}

bool
RXFeatureAPI::tx_pkt(rte_mbuf_t *m) {
    return m_port_mngr->tx_pkt(m);
}

bool
RXFeatureAPI::tx_pkt(const std::string &pkt, uint8_t tx_port_id) {
    return m_port_mngr->m_rx_core->tx_pkt(pkt, tx_port_id);
}

bool
RXFeatureAPI::tx_pkt(rte_mbuf_t *m, uint8_t tx_port_id) {
    return m_port_mngr->m_rx_core->tx_pkt(m, tx_port_id);
}

uint8_t
RXFeatureAPI::get_port_id() {
    return m_port_mngr->m_port_id;
}


//#########################################################

const hr_time_t err_duration_max = ptime_convert_dsec_hr(0.1); // 100 msec before restarting ZMQ

bool  CZmqPacketWriter::flush(){
    if ( m_pkts_cnt == 0 ){
        return false; 
    }
    bool r=false; 
    char * p = &m_buf[0];
    uint32_t pkt_header;
    pkt_header = PAL_NTOHL((ZMQ_HEADER_MAGIC +  (m_pkts_cnt &0xffff) ));
    memcpy(p,(char*)&pkt_header,sizeof(pkt_header));

    // dump the buffer 
    int res;
    res = zmq_send(m_socket, &m_buf[0], m_b_size, ZMQ_DONTWAIT);
    if (res < 0) {
        m_cnt->m_ezmq_tx_to_emu_err++;
        if (errno == EAGAIN){
            if ( err_first_ts == 0 ) {
                err_first_ts = os_get_hr_tick_64();
            } else if ( err_first_ts + err_duration_max > os_get_hr_tick_64() ) {
                r = true;
                m_cnt->m_ezmq_tx_to_emu_restart++;
            }
        }
    }else{
        err_first_ts = 0;
        m_cnt->m_ezmq_tx_to_emu++;
    }

    m_pkts_cnt=0;
    m_b_size=4;
    return (r);
}

    
void CZmqPacketWriter::write_pkt(struct rte_mbuf  *m,uint8_t vport){
    uint32_t pkt_header;
    uint32_t pkt_len = rte_pktmbuf_pkt_len(m);
    if ((pkt_len + m_b_size + 4) > ZMQ_BUFFER_SIZE) {
       flush();
    }

    uint32_t s_pkt_len = pkt_len;

    pkt_header = PAL_NTOHL((0xAA000000 + uint32(vport<<16) + (s_pkt_len &0xffff) ));


    char * p = &m_buf[m_b_size];
    memcpy(p,(char*)&pkt_header,sizeof(pkt_header));
    p+=sizeof(pkt_header);

    while (m) {
        uint16_t seg_len=m->data_len;
        memcpy(p,rte_pktmbuf_mtod(m,char *),seg_len);
        p+=seg_len;
        assert(pkt_len>=seg_len);
        pkt_len-=seg_len;
        m = m->next;
        if (pkt_len==0) {
            assert(m==0);
        }
    }

    m_b_size += (s_pkt_len + 4);
    m_pkts_cnt++;
    if (m_pkts_cnt > 63){
       flush();
    }
}
    
bool CZmqPacketWriter::Create(void * socket,CRxCoreErrCntrs * cnt){
    m_socket = socket;
    m_pkts_cnt =0;
    m_b_size = 4; 
    err_first_ts = 0;
    m_cnt = cnt;
    return true;
}

bool CZmqPacketReader::Create(void * socket, CRxCore * rx,CRxCoreErrCntrs * cnt){
    m_rx = rx;
    m_socket = socket;
    m_cnt = cnt; 
    return false;
}
  
void CZmqPacketReader::tx_buffer(char *pkt,int pkt_size,uint8_t vport) {
        uint8_t port_id = vport;
        rte_mbuf_t *m;
        uint8_t num_ports = CGlobalInfo::m_options.get_expected_ports();
        if ( port_id >= num_ports ) {
            m_cnt->m_ezmq_tx_fe_wrong_vport++;
            return ;
        }
        socket_id_t socket = CGlobalInfo::m_socket.port_to_socket(vport);

        if (pkt_size <= _2048_MBUF_SIZE) {

            m = CGlobalInfo::pktmbuf_alloc_no_assert(socket, pkt_size);
            if ( m ) {
                /* allocate */
                uint8_t *p = (uint8_t *)rte_pktmbuf_append(m, pkt_size);
                assert(p);
                /* copy */
                memcpy(p, pkt, pkt_size);
            }
        }else{
            /*  creating chaning of mbuf in the size of pool */
            rte_mempool *pool_2k = CGlobalInfo::pktmbuf_get_pool(socket, _2048_MBUF_SIZE);
            m = utl_rte_pktmbuf_mem_to_pkt_no_assert(pkt, pkt_size, _2048_MBUF_SIZE, pool_2k);
        }

        if ( !m ) {
            m_cnt->m_ezmq_tx_fe_dropped_no_mbuf++;
            return;
        }

        if (!m_rx->tx_pkt(m, port_id)) {
            m_cnt->m_ezmq_tx_fe_err_send++;
            rte_pktmbuf_free(m);
        }else{
            m_cnt->m_ezmq_tx_fe_ok_send++;
        }
}

int CZmqPacketReader::pool_msg(void){

    int cnt=1;
    while ( cnt > 0 ){
        int len = zmq_recv(m_socket, m_buf,ZMQ_BUFFER_SIZE,ZMQ_NOBLOCK);
        
        if (len < 0 ){
            return 0;
        }else{
            if (len > 0 ) {
                if (len<=ZMQ_BUFFER_SIZE) {
                   parse_msg(m_buf,len);
                }else{
                    m_cnt->m_ezmq_rx_fe_err_buffer_len_high++;
                    break;
                }
            }else{
                m_cnt->m_ezmq_rx_fe_err_len_zero++;
                break;
            }
        }
        cnt--;
    }
    return (0);
}


int CZmqPacketReader::parse_msg(char *buf,int size){
    char *p = buf;
    if (size < 4) {
        return -1;
    }
    uint32_t header = PAL_NTOHL(*((uint32*)p));
    if ((header& 0xFFFF0000) != ZMQ_HEADER_MAGIC ){
        return -1;
    }
    uint16_t pkts = uint16_t((header&0xffff));
    p+=sizeof(header);
    size-=sizeof(header);

    int i;
    for (i=0; i<(int)pkts; i++){
        if (size < 4) {
            return -1;
        }
        uint32_t pkt_header = PAL_NTOHL(*((uint32*)p));
        
        if ((pkt_header& 0xFF000000) != ZMQ_PKT_HEADER_MAGIC ){
            return -1;
        }
        uint8_t vport = uint8_t(((pkt_header& 0x00FF0000)>>16));
        uint16_t pkt_size = uint16_t(((pkt_header& 0xFFFF)));

        size -= sizeof(pkt_header);
        p += sizeof(pkt_header);

        if (size < int(pkt_size)) {
            return -1;
        }

        tx_buffer(p,int(pkt_size),vport);

        size-=int(pkt_size);
        p += int(pkt_size);
    }
    if (size !=0) {
        // counter 
    }
    return 0;
}









