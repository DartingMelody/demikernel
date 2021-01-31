// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "lwip_queue.hh"

#include <arpa/inet.h>
#include <boost/chrono.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <cassert>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dmtr/annot.h>
#include <dmtr/cast.h>
#include <dmtr/latency.h>
#include <dmtr/libos.h>
#include <dmtr/sga.h>
#include <iostream>
#include <dmtr/libos/mem.h>
#include <dmtr/libos/raii_guard.hh>
#include <netinet/in.h>
#include <rte_memzone.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_eal.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_memcpy.h>
#include <rte_udp.h>
#include <unistd.h>
#include <sys/mman.h>

#include <functional>

#define PAGE_SIZE sysconf(_SC_PAGE_SIZE)
#define NUM_EXTERNAL_PAGES 1000
namespace bpo = boost::program_options;
//#define DMTR_NO_SER
//#define DMTR_ALLOCATE_SEGMENTS

#define FILL_CHAR   'a'
#define NUM_MBUFS               8191
#define MBUF_CACHE_SIZE         250
#define RX_RING_SIZE            2048
#define TX_RING_SIZE            2048
#define IP_DEFTTL  64   /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN  0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)
// #define DMTR_DEBUG 1
//#define DMTR_PROFILE 1
//#define MBUF_BUF_SIZE RTE_MBUF_DEFAULT_BUF_SIZE
// default mbuf size is ->RTE_MBUF_DEFAULT_DATAROOM + RTE_PKTMBUF_HEADROOM
#define MBUF_BUF_SIZE RTE_ETHER_MAX_JUMBO_FRAME_LEN + RTE_PKTMBUF_HEADROOM
#define RX_PACKET_LEN 9216
//#define RX_PACKET_LEN RTE_ETHER_MAX_LEN  
/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH          8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH          8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH          0 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH          0 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH          0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH          0  /**< Default values of TX write-back threshold reg. */

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT    128
#define RTE_TEST_TX_DESC_DEFAULT    128
#if DMTR_PROFILE
typedef std::unique_ptr<dmtr_latency_t, std::function<void(dmtr_latency_t *)>> latency_ptr_type;
static latency_ptr_type read_latency;
static latency_ptr_type write_latency;
#endif
int dmtr::lwip_queue::num_sent = 0;
sockaddr_in *dmtr::lwip_queue::my_bound_src = NULL;
sockaddr_in *dmtr::lwip_queue::default_src = NULL;
uint64_t dmtr::lwip_queue::in_packets = 0;
uint64_t dmtr::lwip_queue::out_packets = 0;
uint64_t dmtr::lwip_queue::invalid_packets = 0;
bool dmtr::lwip_queue::zero_copy_mode = false;
lwip_sga_t global_lwip_sga;
lwip_sga_t *dmtr::lwip_queue::lwip_sga = &global_lwip_sga;
uint32_t dmtr::lwip_queue::current_segment_size = 0;
bool dmtr::lwip_queue::current_has_header = true;
bool dmtr::lwip_queue::use_external_memory = false;

ext_mem_cfg_t global_ext_mem_cfg;
ext_mem_cfg_t *dmtr::lwip_queue::ext_mem_cfg = &global_ext_mem_cfg;

const struct rte_ether_addr dmtr::lwip_queue::ether_broadcast = {
    .addr_bytes = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}
};


lwip_addr::lwip_addr(const struct sockaddr_in &addr)
    : addr(addr)
{
    this->addr = addr;
}

lwip_addr::lwip_addr()
{
    memset((void *)&addr, 0, sizeof(addr));
}

bool
operator==(const lwip_addr &a,
           const lwip_addr &b)
{
    if (a.addr.sin_addr.s_addr == INADDR_ANY || b.addr.sin_addr.s_addr == INADDR_ANY) {
        return true;
    } else {
        return (a.addr.sin_addr.s_addr == b.addr.sin_addr.s_addr) &&
            (a.addr.sin_port == b.addr.sin_port);
    }
}

bool
operator!=(const lwip_addr &a,
           const lwip_addr &b)
{
    return !(a == b);
}

bool
operator<(const lwip_addr &a,
          const lwip_addr &b)
{
    return (memcmp(&a.addr, &b.addr, sizeof(a.addr)) < 0);
}

struct rte_mempool *dmtr::lwip_queue::our_mbuf_pool = NULL;
struct rte_mempool *dmtr::lwip_queue::header_mbuf_pool = NULL;
struct rte_mempool *dmtr::lwip_queue::payload_mbuf_pool1 = NULL;
struct rte_mempool *dmtr::lwip_queue::payload_mbuf_pool2 = NULL;
struct rte_mempool *dmtr::lwip_queue::extbuf_mbuf_pool = NULL;
const struct rte_memzone *dmtr::lwip_queue::application_memzone = NULL;
bool dmtr::lwip_queue::our_dpdk_init_flag = false;
// local ports bound for incoming connections, used to demultiplex incoming new messages for accept
std::map<lwip_addr, std::queue<dmtr_sgarray_t> *> dmtr::lwip_queue::our_recv_queues;
std::unordered_map<std::string, struct in_addr> dmtr::lwip_queue::our_mac_to_ip_table;
std::unordered_map<in_addr_t, struct rte_ether_addr> dmtr::lwip_queue::our_ip_to_mac_table;

int dmtr::lwip_queue::ip_to_mac(struct rte_ether_addr &mac_out, const struct in_addr &ip)
{
    auto it = our_ip_to_mac_table.find(ip.s_addr);
    DMTR_TRUE(ENOENT, our_ip_to_mac_table.cend() != it);
    mac_out = it->second;
    return 0;
}

int dmtr::lwip_queue::mac_to_ip(struct in_addr &ip_out, const struct rte_ether_addr &mac)
{
    std::string mac_s(reinterpret_cast<const char *>(mac.addr_bytes), RTE_ETHER_ADDR_LEN);
    auto it = our_mac_to_ip_table.find(mac_s);
    DMTR_TRUE(ENOENT, our_mac_to_ip_table.cend() != it);
    ip_out = it->second;
    return 0;
}

bool
dmtr::lwip_queue::insert_recv_queue(const lwip_addr &saddr,
                                    const dmtr_sgarray_t &sga)
{
    auto it = our_recv_queues.find(saddr);
    if (it == our_recv_queues.end()) {
        return false;
    }
    it->second->push(sga);
    return true;
}

int dmtr::lwip_queue::ip_sum(uint16_t &sum_out, const uint16_t *hdr, int hdr_len) {
    DMTR_NOTNULL(EINVAL, hdr);
    uint32_t sum = 0;

    while (hdr_len > 1) {
        sum += *hdr++;
        if (sum & 0x80000000) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        hdr_len -= 2;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    sum_out = ~sum;
    return 0;
}

int dmtr::lwip_queue::print_ether_addr(FILE *f, struct rte_ether_addr &eth_addr) {
    DMTR_NOTNULL(EINVAL, f);

    char buf[RTE_ETHER_ADDR_FMT_SIZE];
    rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, &eth_addr);
    fputs(buf, f);
    return 0;
}

int dmtr::lwip_queue::print_link_status(FILE *f, uint16_t port_id, const struct rte_eth_link *link) {
    DMTR_NOTNULL(EINVAL, f);
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    struct rte_eth_link link2 = {};
    if (NULL == link) {
        DMTR_OK(rte_eth_link_get_nowait(port_id, link2));
        link = &link2;
    }
    if (ETH_LINK_UP == link->link_status) {
        const char * const duplex = ETH_LINK_FULL_DUPLEX == link->link_duplex ?  "full" : "half";
        fprintf(f, "Port %d Link Up - speed %u " "Mbps - %s-duplex\n", port_id, link->link_speed, duplex);
    } else {
        printf("Port %d Link Down\n", port_id);
    }

    return 0;
}

int dmtr::lwip_queue::wait_for_link_status_up(uint16_t port_id)
{
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    const size_t sleep_duration_ms = 100;
    const size_t retry_count = 90;

    struct rte_eth_link link = {};
    for (size_t i = 0; i < retry_count; ++i) {
        DMTR_OK(rte_eth_link_get_nowait(port_id, link));
        if (ETH_LINK_UP == link.link_status) {
            DMTR_OK(print_link_status(stderr, port_id, &link));
            return 0;
        }

        rte_delay_ms(sleep_duration_ms);
    }

    DMTR_OK(print_link_status(stderr, port_id, &link));
    return ECONNREFUSED;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
int dmtr::lwip_queue::init_dpdk_port(uint16_t port_id, struct rte_mempool &mbuf_pool) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    const uint16_t rx_rings = 1;
    const uint16_t tx_rings = 1;
    const uint16_t nb_rxd = RX_RING_SIZE;
    const uint16_t nb_txd = TX_RING_SIZE;
    uint16_t mtu;
    
    struct ::rte_eth_dev_info dev_info = {};
    DMTR_OK(rte_eth_dev_info_get(port_id, dev_info));
    DMTR_OK(rte_eth_dev_set_mtu(port_id, RX_PACKET_LEN)); 
    DMTR_OK(rte_eth_dev_get_mtu(port_id, &mtu));
    std::cerr << "Dev info MTU: " << mtu << std::endl;
    struct ::rte_eth_conf port_conf = {};
    port_conf.rxmode.max_rx_pkt_len = RX_PACKET_LEN;
            
    port_conf.rxmode.offloads = DEV_RX_OFFLOAD_JUMBO_FRAME;
    port_conf.txmode.offloads = DEV_TX_OFFLOAD_MULTI_SEGS;
//    port_conf.rxmode.mq_mode = ETH_MQ_RX_RSS;
//    port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP | dev_info.flow_type_rss_offloads;
    port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;

    struct ::rte_eth_rxconf rx_conf = {};
    rx_conf.rx_thresh.pthresh = RX_PTHRESH;
    rx_conf.rx_thresh.hthresh = RX_HTHRESH;
    rx_conf.rx_thresh.wthresh = RX_WTHRESH;
    rx_conf.rx_free_thresh = 32;

    struct ::rte_eth_txconf tx_conf = {};
    tx_conf.tx_thresh.pthresh = TX_PTHRESH;
    tx_conf.tx_thresh.hthresh = TX_HTHRESH;
    tx_conf.tx_thresh.wthresh = TX_WTHRESH;
    tx_conf.tx_free_thresh = 32;

    // configure the ethernet device.
    DMTR_OK(rte_eth_dev_configure(port_id, rx_rings, tx_rings, port_conf));

    // todo: what does this do?
/*
    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0) {
        return retval;
    }
*/

    // todo: this call fails and i don't understand why.
    int socket_id = 0;
    int ret = rte_eth_dev_socket_id(socket_id, port_id);
    if (0 != ret) {
        fprintf(stderr, "WARNING: Failed to get the NUMA socket ID for port %d.\n", port_id);
        socket_id = 0;
    }

    // allocate and set up 1 RX queue per Ethernet port.
    for (uint16_t i = 0; i < rx_rings; ++i) {
        DMTR_OK(rte_eth_rx_queue_setup(port_id, i, nb_rxd, socket_id, rx_conf, mbuf_pool));
    }

    // allocate and set up 1 TX queue per Ethernet port.
    for (uint16_t i = 0; i < tx_rings; ++i) {
        DMTR_OK(rte_eth_tx_queue_setup(port_id, i, nb_txd, socket_id, tx_conf));
    }

    // start the ethernet port.
    DMTR_OK(rte_eth_dev_start(port_id));

    //DMTR_OK(rte_eth_promiscuous_enable(port_id));

    // disable the rx/tx flow control
    // todo: why?
    struct ::rte_eth_fc_conf fc_conf = {};
    DMTR_OK(rte_eth_dev_flow_ctrl_get(port_id, fc_conf));
    fc_conf.mode = RTE_FC_NONE;
    DMTR_OK(rte_eth_dev_flow_ctrl_set(port_id, fc_conf));

    DMTR_OK(wait_for_link_status_up(port_id));

    return 0;
}

int dmtr::lwip_queue::init_dpdk(int argc, char *argv[])
{
    DMTR_TRUE(ERANGE, argc >= 0);
    if (argc > 0) {
        DMTR_NOTNULL(EINVAL, argv);
    }
    DMTR_TRUE(EPERM, !our_dpdk_init_flag);

    std::string config_path;
    bpo::options_description desc("Allowed options");
    desc.add_options()
        ("help", "display usage information")
        ("config-path", bpo::value<std::string>(&config_path)->default_value("/demikernel/config.yaml"), "specify configuration file");

    bpo::variables_map vm;
    bpo::store(bpo::command_line_parser(argc, argv).options(desc).allow_unregistered().run(), vm);
    bpo::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    if (access(config_path.c_str(), R_OK) == -1) {
        std::cerr << "Unable to find config file at `" << config_path << "`." << std::endl;
        return ENOENT;
    }

    std::vector<std::string> init_args;
    YAML::Node config = YAML::LoadFile(config_path);
    YAML::Node node = config["dpdk"]["eal_init"];
    if (YAML::NodeType::Sequence == node.Type()) {
        init_args = node.as<std::vector<std::string>>();
    }
    std::cerr << "eal_init: [";
    std::vector<char *> init_cargs;
    for (auto i = init_args.cbegin(); i != init_args.cend(); ++i) {
        if (i != init_args.cbegin()) {
            std::cerr << ", ";
        }
        std::cerr << "\"" << *i << "\"";
        init_cargs.push_back(const_cast<char *>(i->c_str()));
    }
    std::cerr << "]" << std::endl;

    int unused = -1;
    DMTR_OK(rte_eal_init(unused, init_cargs.size(), init_cargs.data()));
    return finish_dpdk_init(config);
}

int dmtr::lwip_queue::finish_dpdk_init(YAML::Node &config)
{
    
#if DMTR_PROFILE
    if (NULL == read_latency) {
        dmtr_latency_t *l;
        DMTR_OK(dmtr_new_latency(&l, "read"));
        read_latency = latency_ptr_type(l, [](dmtr_latency_t *latency) {
            dmtr_dump_latency(stderr, latency);
            dmtr_delete_latency(&latency);
        });
    }

    if (NULL == write_latency) {
        dmtr_latency_t *l;
        DMTR_OK(dmtr_new_latency(&l, "write"));
        write_latency = latency_ptr_type(l, [](dmtr_latency_t *latency) {
            dmtr_dump_latency(stderr, latency);
            dmtr_delete_latency(&latency);
        });
    }
#endif

    YAML::Node node = config["lwip"]["known_hosts"];
    if (YAML::NodeType::Map == node.Type()) {
        for (auto i = node.begin(); i != node.end(); ++i) {
            auto mac = i->first.as<std::string>();
            auto ip = i->second.as<std::string>();
            DMTR_OK(learn_addrs(mac.c_str(), ip.c_str()));
        }
    }

    const uint16_t nb_ports = rte_eth_dev_count_avail();
    DMTR_TRUE(ENOENT, nb_ports > 0);
    fprintf(stderr, "DPDK reports that %d ports (interfaces) are available.\n", nb_ports);

    // create pool of memory for ring buffers.
    struct rte_mempool *mbuf_pool = NULL;
    DMTR_OK(rte_pktmbuf_pool_create(
                                    mbuf_pool,
                                    "default_mbuf_pool",
                                    NUM_MBUFS * nb_ports,
                                    MBUF_CACHE_SIZE,
                                    0,
                                    MBUF_BUF_SIZE,
                                    rte_socket_id()));

    // initialize all ports.
    uint16_t i = 0;
    uint16_t port_id = 0;
    RTE_ETH_FOREACH_DEV(i) {
        DMTR_OK(init_dpdk_port(i, *mbuf_pool));
        port_id = i;
    }

    if (rte_lcore_count() > 1) {
        printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");
    }

    our_dpdk_init_flag = true;
    our_dpdk_port_id = port_id;
    our_mbuf_pool = mbuf_pool;

    // set up a default address
    node = config["catnip"]["my_ipv4_addr"];
    if (YAML::NodeType::Scalar == node.Type()) {
        auto ipv4_addr = node.as<std::string>();
        struct in_addr in_addr = {};
        if (inet_pton(AF_INET, ipv4_addr.c_str(), &in_addr) != 1) {
            std::cerr << "Unable to parse IP address." << std::endl;
            return EINVAL;
        }
        default_src = new struct sockaddr_in();
        default_src->sin_addr.s_addr = in_addr.s_addr;
    }

    return 0;
}

int dmtr::lwip_queue::init_ext_mem(void *mmap_addr, uint16_t *mmap_len) {
    assert(*mmap_len % PAGE_SIZE == 0);
    assert(mmap_addr != NULL);
    
    // first, mmap some memory: let's say like 100 pages
    /*size_t page_size = PAGE_SIZE;
    size_t num_pages = NUM_EXTERNAL_PAGES;
    void * addr = mmap(NULL, page_size * num_pages, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if ( addr == MAP_FAILED ) {
        printf("Failed to mmap memory\n");
        return 1;
    }

    // second, register the external memory
    int ret = rte_extmem_register(mmap_addr, *mmap_len, NULL, 0, PAGE_SIZE);
    if (ret != 0) {
        std::cout << "Register Errno: " << rte_strerror(rte_errno) << std::endl;
        return ret;
    }

    // third, need to map the external memory per port
    uint16_t pid = 0;
    RTE_ETH_FOREACH_DEV(pid) {
        struct rte_eth_dev *dev = &rte_eth_devices[pid];
        ret = rte_dev_dma_map(dev->device, mmap_addr, 0, *mmap_len);
        if (ret != 0) {
            std::cout << "dma map errno: " << rte_strerror(rte_errno) << std::endl;
            return ret;
        }
    }*/
    ext_mem_cfg->addr = mmap_addr;
    ext_mem_cfg->num_pages = (*mmap_len / PAGE_SIZE);
    struct ::rte_mbuf_ext_shared_info* shinfo = ::rte_pktmbuf_ext_shinfo_init_helper(mmap_addr, mmap_len, free_external_buffer_callback, NULL);
    ext_mem_cfg->shinfo = shinfo;
    ext_mem_cfg->buf_size = *mmap_len;
    ext_mem_cfg->ext_mem_iova = rte_malloc_virt2iova(mmap_addr);
    if (ext_mem_cfg->ext_mem_iova == RTE_BAD_IOVA) {
        printf("Failed to get iova\n");
        return EINVAL;
    }
    return 0;

}

void dmtr::lwip_queue::free_external_buffer_callback(void *addr, void *opaque) {
    if (!use_external_memory) {
        printf("Addr of mmap'd memory: %p\n", ext_mem_cfg->addr);
        printf("In this function: addr is %p\n", addr);
    }
    // do nothing, for now
}

const size_t dmtr::lwip_queue::our_max_queue_depth = 1024;
boost::optional<uint16_t> dmtr::lwip_queue::our_dpdk_port_id;

dmtr::lwip_queue::lwip_queue(int qd) :
    io_queue(NETWORK_Q, qd),
    my_listening_flag(false)
{}

int dmtr::lwip_queue::new_object(std::unique_ptr<io_queue> &q_out, int qd) {
    q_out = NULL;
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    q_out = std::unique_ptr<io_queue>(new lwip_queue(qd));
    DMTR_NOTNULL(ENOMEM, q_out);
    return 0;
}

dmtr::lwip_queue::~lwip_queue()
{
    int ret = close();
    if (0 != ret) {
        std::ostringstream msg;
        msg << "Failed to close `lwip_queue` object (error " << ret << ")." << std::endl;
        DMTR_PANIC(msg.str().c_str());
    }
    if (is_bound()) {
        std::cerr << "Bound to addr: " << my_bound_src->sin_addr.s_addr << ":" << my_bound_src->sin_port << std::endl;

        free(my_bound_src);
    }
    if (is_connected()) {
        std::cerr << "Connected to addr: " << my_default_dst.get().sin_addr.s_addr << ":" << my_default_dst.get().sin_port << std::endl;
    }

    rte_eth_stats stats{};
    if (rte_eth_stats_get(our_dpdk_port_id.get(), &stats) == 0) {
        fprintf(stderr, "DMTR TOTAL STATS in=%lu out=%lu invalid=%lu\n DMTR QUEUE STATS in=%lu out=%lu\nDPDK STATS: in=%lu out=%lu missed=%lu in_errors=%lu out_errors=%lu\n",
                in_packets, out_packets, invalid_packets,
                q_in_packets, q_out_packets,
                stats.ipackets, stats.opackets, stats.imissed, stats.ierrors, stats.oerrors);
    }
}

int dmtr::lwip_queue::socket(int domain, int type, int protocol) {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);

    // we don't currently support anything but UDP and faux-TCP.
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        return ENOTSUP;
    }

    return 0;
}

int
dmtr::lwip_queue::getsockname(struct sockaddr * const saddr, socklen_t * const size) {
    DMTR_NOTNULL(EINVAL, size);
    DMTR_TRUE(ENOMEM, *size >= sizeof(struct sockaddr_in));
    DMTR_TRUE(EINVAL, !is_bound());

    struct sockaddr_in *saddr_in = reinterpret_cast<struct sockaddr_in *>(saddr);
    *saddr_in = *my_bound_src;
    return 0;
}

int dmtr::lwip_queue::accept(std::unique_ptr<io_queue> &q_out, dmtr_qtoken_t qt, int new_qd)
{
    q_out = NULL;
    DMTR_TRUE(EPERM, my_listening_flag);
    DMTR_NOTNULL(EINVAL, my_accept_thread);

    auto * const q = new lwip_queue(new_qd);
    DMTR_TRUE(ENOMEM, q != NULL);
    auto qq = std::unique_ptr<io_queue>(q);

    DMTR_OK(new_task(qt, DMTR_OPC_ACCEPT, q));
    my_accept_thread->enqueue(qt);
#if DMTR_DEBUG
    printf("waiting for accept: %u\n", qt);
#endif
    
    q_out = std::move(qq);
    return 0;
}

int dmtr::lwip_queue::accept_thread(task::thread_type::yield_type &yield, task::thread_type::queue_type &tq) {
    DMTR_TRUE(EINVAL, good());
    DMTR_TRUE(EINVAL, my_listening_flag);

    while (good()) {
        while (tq.empty()) {
            yield();
        }

        auto qt = tq.front();
        tq.pop();
        task *t;
        DMTR_OK(get_task(t, qt));

        io_queue *new_q = NULL;
        DMTR_TRUE(EINVAL, t->arg(new_q));
        auto * const new_lq = dynamic_cast<lwip_queue *>(new_q);
        DMTR_NOTNULL(EINVAL, new_lq);

        while (my_recv_queue->empty()) {
            if (service_incoming_packets() == EAGAIN ||
                my_recv_queue->empty())
                yield();
        }

        dmtr_sgarray_t &sga = my_recv_queue->front();
        // todo: `my_recv_queue->pop()` should be called from a `raii_guard`.
        sockaddr_in &src = sga.sga_addr;
        lwip_addr addr = lwip_addr(src);
        new_lq->my_default_dst = src;
        new_lq->my_recv_queue = our_recv_queues[addr];
        new_lq->start_threads();
        my_recv_queue->pop();
        DMTR_OK(t->complete(0, new_lq->qd(), src));
    }

    return 0;
}

int dmtr::lwip_queue::listen(int backlog)
{
    DMTR_TRUE(EPERM, !my_listening_flag);
    DMTR_TRUE(EINVAL, is_bound());
    //    std::cout << "Listening ..." << std::endl;
    my_listening_flag = true;
    start_threads();
    return 0;
}

int dmtr::lwip_queue::bind(const struct sockaddr * const saddr, socklen_t size) {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EINVAL, !is_bound());
    DMTR_NOTNULL(EINVAL, saddr);
    DMTR_TRUE(EINVAL, sizeof(struct sockaddr_in) == size);
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);
    // only one socket can be bound to an address at a time
    const uint16_t dpdk_port_id = boost::get(our_dpdk_port_id);

    struct rte_ether_addr mac = {};
    DMTR_OK(rte_eth_macaddr_get(dpdk_port_id, mac));
    struct in_addr ip;
    DMTR_OK(mac_to_ip(ip, mac));

    struct sockaddr_in saddr_copy =
        *reinterpret_cast<const struct sockaddr_in *>(saddr);
    DMTR_NONZERO(EINVAL, saddr_copy.sin_port);

    if (INADDR_ANY == saddr_copy.sin_addr.s_addr) {
        saddr_copy.sin_addr = ip;
    } else {
        // we cannot deviate from associations found in `config.yaml`.
        DMTR_TRUE(EPERM, 0 == memcmp(&saddr_copy.sin_addr, &ip, sizeof(ip)));
    }
    DMTR_TRUE(EINVAL, our_recv_queues.find(lwip_addr(saddr_copy)) == our_recv_queues.end());
    my_bound_src = reinterpret_cast<sockaddr_in *>(malloc(size));
    *my_bound_src = saddr_copy;
    std::queue<dmtr_sgarray_t> *listening = new std::queue<dmtr_sgarray_t>();
    our_recv_queues[lwip_addr(saddr_copy)] = listening;
    my_recv_queue = listening;
#if DMTR_DEBUG
    std::cout << "Binding to addr: " << saddr_copy.sin_addr.s_addr << ":" << saddr_copy.sin_port << std::endl;
#endif
    return 0;
}

int dmtr::lwip_queue::connect(dmtr_qtoken_t qt, const struct sockaddr * const saddr, socklen_t size) {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EINVAL, sizeof(struct sockaddr_in) == size);
    DMTR_TRUE(EPERM, !is_bound());
    DMTR_TRUE(EPERM, !is_connected());
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);
    const uint16_t dpdk_port_id = boost::get(our_dpdk_port_id);

    my_default_dst = *reinterpret_cast<const struct sockaddr_in *>(saddr);
    struct sockaddr_in saddr_copy =
        *reinterpret_cast<const struct sockaddr_in *>(saddr);
    DMTR_NONZERO(EINVAL, saddr_copy.sin_port);
    DMTR_NONZERO(EINVAL, saddr_copy.sin_addr.s_addr);
    DMTR_TRUE(EINVAL, saddr_copy.sin_family == AF_INET);
    std::queue<dmtr_sgarray_t> *q = new std::queue<dmtr_sgarray_t>();
    our_recv_queues[lwip_addr(saddr_copy)] = q;
    my_recv_queue = q;

    // give the connection the local ip;
    struct rte_ether_addr mac;
    DMTR_OK(rte_eth_macaddr_get(dpdk_port_id, mac));
    struct sockaddr_in src = {};
    src.sin_family = AF_INET;
    src.sin_port = htons(12345);
    DMTR_OK(mac_to_ip(src.sin_addr, mac));
    if (src.sin_addr.s_addr == 0) {
        src.sin_addr.s_addr = default_src->sin_addr.s_addr;
    }
    my_bound_src = reinterpret_cast<sockaddr_in *>(malloc(size));
    *my_bound_src = src;

    char src_ip_str[INET_ADDRSTRLEN], dst_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(my_bound_src->sin_addr), src_ip_str, sizeof(src_ip_str));
    inet_ntop(AF_INET, &(my_default_dst->sin_addr), dst_ip_str, sizeof(dst_ip_str));
    std::cout << "Connecting from " << src_ip_str << " to " << dst_ip_str << std::endl;

    start_threads();

    DMTR_OK(new_task(qt, DMTR_OPC_CONNECT));
    task *t;
    DMTR_OK(get_task(t, qt));
    DMTR_OK(t->complete(0));
    return 0;
}

int dmtr::lwip_queue::close() {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    if (!is_connected()) {
        return 0;
    }
    my_default_dst = boost::none;
    delete my_recv_queue;
    return 0;
}

int dmtr::lwip_queue::push(dmtr_qtoken_t qt, const dmtr_sgarray_t &sga) {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);
    DMTR_NOTNULL(EINVAL, my_push_thread);

    DMTR_OK(new_task(qt, DMTR_OPC_PUSH, sga));
    my_push_thread->enqueue(qt);
    my_push_thread->service();
    return 0;
}

int dmtr::lwip_queue::push_thread(task::thread_type::yield_type &yield, task::thread_type::queue_type &tq)  {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);
    const uint16_t dpdk_port_id = *our_dpdk_port_id;

    while (good()) {
        while (tq.empty()) {
            yield();
        }

        auto qt = tq.front();
        tq.pop();
        task *t;
        DMTR_OK(get_task(t, qt));

        const dmtr_sgarray_t *sga = NULL;
        DMTR_TRUE(EINVAL, t->arg(sga));

        size_t sgalen = 0;
        DMTR_OK(dmtr_sgalen(&sgalen, sga));
        if (0 == sgalen && !(zero_copy_mode)) {
            DMTR_OK(t->complete(ENOMSG));
            // move onto the next task.
            continue;
        }

        const struct sockaddr_in *saddr = NULL;
        if (!is_connected()) {
            saddr = &sga->sga_addr;
        } else {
            saddr = &boost::get(my_default_dst);
            //std::cout << "Sending to default address: " << saddr->sin_addr.s_addr << std::endl;
        }
        struct rte_mbuf *pkt = NULL;
        if (use_external_memory) {
            lwip_sga->num_segments = 2;
        }
        struct rte_mbuf *pkts[lwip_sga->num_segments];
        if (zero_copy_mode) {
            DMTR_OK(rte_pktmbuf_alloc(pkts[0], header_mbuf_pool));
            pkts[0]->data_len = 0;
            pkts[0]->pkt_len = 0;
            for (size_t i = 1; i < lwip_sga->num_segments; i++) {
                if (i < (lwip_sga->total_size % lwip_sga->num_segments)) {
                    // larger segment size
                    DMTR_OK(rte_pktmbuf_alloc(pkts[i], payload_mbuf_pool2));
                } else {
                    // smaller segment size
                    DMTR_OK(rte_pktmbuf_alloc(pkts[i], payload_mbuf_pool1));
                }
                pkts[i]->data_len = 0;
                pkts[i]->pkt_len = 0;
            }
            pkt = pkts[0];
        } else {
            if (use_external_memory) {
                num_sent++;
                DMTR_OK(rte_pktmbuf_alloc(pkts[0], our_mbuf_pool));
                DMTR_OK(rte_pktmbuf_alloc(pkts[1], extbuf_mbuf_pool));
                // TODO: should packet be attached to entire buffer
                char *payload = reinterpret_cast<char *>(sga->sga_segs[1].sgaseg_buf);
                ::rte_pktmbuf_attach_extbuf(pkts[1], (void *)payload, ext_mem_cfg->ext_mem_iova, sga->sga_segs[1].sgaseg_len, ext_mem_cfg->shinfo);
                pkts[0]->next = pkts[1];
                //pkt = pkts[0];
            } else {
                DMTR_OK(rte_pktmbuf_alloc(pkt, our_mbuf_pool));
            }
        }
        auto *p = rte_pktmbuf_mtod(pkt, uint8_t *);

        // packet layout order is (from outside -> in):
        // ether_hdr
        // ipv4_hdr
        // udp_hdr
        // sga.num_bufs
        // sga.buf[0].len
        // sga.buf[0].buf
        // sga.buf[1].len
        // sga.buf[1].buf
        // ...

        // First, compute the offset of each header.  We will later fill them in, in reverse order.
        auto * const eth_hdr = reinterpret_cast<struct ::rte_ether_hdr *>(p);
        p += sizeof(*eth_hdr);
        auto * const ip_hdr = reinterpret_cast<struct ::rte_ipv4_hdr *>(p);
        p += sizeof(*ip_hdr);
        auto * const udp_hdr = reinterpret_cast<struct ::rte_udp_hdr *>(p);
        p += sizeof(*udp_hdr);

        // write in the packet ID now in the header as well
        auto * const id = reinterpret_cast<uint32_t *>(p);
        *id = htonl(sga->id);
        p += sizeof(*id);

        // auto *data_ptr = p;

        // now add in the SGA id to be sent and deserialized on the other side.
        uint32_t total_len = sizeof(*id); // Length of data written so far.

#ifdef DMTR_NO_SER
        for (size_t i = 0; i < sga->sga_numsegs; i++) {
            const auto len = sga->sga_segs[i].sgaseg_len;
            rte_memcpy(p, sga->sga_segs[i].sgaseg_buf, len);
            total_len += len;
            p += len;
        }
#else
        if (zero_copy_mode) {
            for (uint32_t i = 0; i < lwip_sga->num_segments; i++) {
                total_len += lwip_sga->segment_sizes[i];
            }
        } else if (use_external_memory) {
            // copy in the first sga into the header
            rte_memcpy(p, sga->sga_segs[0].sgaseg_buf, sga->sga_segs[0].sgaseg_len);
            for (size_t i = 0; i < sga->sga_numsegs; i++) {
                total_len += sga->sga_segs[i].sgaseg_len;
            }
        } else {
            // Fill in Demeter data at p.
            /*{
                auto * const u32 = reinterpret_cast<uint32_t *>(p);
                *u32 = htonl(sga->sga_numsegs);
                total_len += sizeof(*u32);
                p += sizeof(*u32);
            }*/

            for (size_t i = 0; i < sga->sga_numsegs; i++) {
                // just flatten it into a single contiguous buffer, no extra
                // overhead
                /*auto * const u32 = reinterpret_cast<uint32_t *>(p);
                const auto len = sga->sga_segs[i].sgaseg_len;
                *u32 = htonl(len);
                total_len += sizeof(*u32);
                p += sizeof(*u32);*/
                // todo: remove copy by associating foreign memory with
                // pktmbuf object.
                rte_memcpy(p, sga->sga_segs[i].sgaseg_buf, sga->sga_segs[i].sgaseg_len);
                total_len += sga->sga_segs[i].sgaseg_len;
                p += sga->sga_segs[i].sgaseg_len;
            }
        }
#endif
        size_t header_size = 0;
        // Fill in UDP header.
        {
            memset(udp_hdr, 0, sizeof(*udp_hdr));

            // sin_port is already in network byte order.
            const in_port_t dst_port = saddr->sin_port;
            // todo: need a way to get my own IP address even if `bind()` wasn't
            // called.
            const in_port_t src_port = is_bound() ? my_bound_src->sin_port : dst_port;

            uint16_t udp_len = 0; // In host byte order.
            DMTR_OK(dmtr_u32tou16(&udp_len, total_len + sizeof(*udp_hdr)));

            // Already in network byte order.
            udp_hdr->src_port = src_port;
            udp_hdr->dst_port = dst_port;

            udp_hdr->dgram_len = htons(udp_len);
            udp_hdr->dgram_cksum = 0;
            header_size += sizeof(*udp_hdr);
            total_len += sizeof(*udp_hdr);
        }

        // Fill in IP header.
        {
            memset(ip_hdr, 0, sizeof(*ip_hdr));

            uint16_t ip_len = 0; // In host byte order.
            DMTR_OK(dmtr_u32tou16(&ip_len, total_len + sizeof(*ip_hdr)));

            struct in_addr src_ip;
            // todo: need a way to get my own IP address even if `bind()` wasn't
            // called.
            if (is_bound()) {
                src_ip = my_bound_src->sin_addr;
            } else {
                DMTR_OK(mac_to_ip(src_ip, eth_hdr->s_addr));
            }

            ip_hdr->version_ihl = IP_VHL_DEF;
            ip_hdr->total_length = htons(ip_len);
            ip_hdr->time_to_live = IP_DEFTTL;
            ip_hdr->next_proto_id = IPPROTO_UDP;
            // The s_addr field is already in network byte order.
            ip_hdr->src_addr = src_ip.s_addr;
            ip_hdr->dst_addr = saddr->sin_addr.s_addr;

            uint16_t checksum = 0;
            DMTR_OK(ip_sum(checksum, reinterpret_cast<uint16_t *>(ip_hdr), sizeof(*ip_hdr)));
            // The checksum is computed on the raw header and is already in the correct byte order.
            ip_hdr->hdr_checksum = checksum;

            header_size += sizeof(*ip_hdr);
            total_len += sizeof(*ip_hdr);
        }

        // Fill in  Ethernet header
        {
            memset(eth_hdr, 0, sizeof(*eth_hdr));

            DMTR_OK(ip_to_mac(/* out */ eth_hdr->d_addr, saddr->sin_addr));
            rte_eth_macaddr_get(dpdk_port_id, /* out */ eth_hdr->s_addr);
            eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

            header_size += sizeof(*eth_hdr);
            total_len += sizeof(*eth_hdr);
        }

        // add in the sga ID to the "header size" to record data_len
        header_size += sizeof(*id);

        if (zero_copy_mode) {
            pkt->data_len = lwip_sga->segment_sizes[0] + header_size;
            pkt->pkt_len = total_len;
            pkt->nb_segs = lwip_sga->num_segments;
            struct rte_mbuf* cur_pkt = pkt;
            //printf("Segment 0 data_len: %u\n", pkt->data_len);
            //char *payload = reinterpret_cast<char *>(data_ptr);
            //size_t payload_length = pkt->data_len - header_size;
            //printf("0th segment, first byte of payload: %c, 2nd: %c, 2nd to last: %c, last: %c\n", payload[0], payload[1], payload[payload_length - 2], payload[payload_length - 1]);
            for (size_t i = 1; i < lwip_sga->num_segments; i++) {
                struct rte_mbuf* prev = cur_pkt;
                cur_pkt = pkts[i];
                prev->next = cur_pkt;
                cur_pkt->data_len = lwip_sga->segment_sizes[i];
                //printf("Segment %zu data_len: %u\n", i, cur_pkt->data_len);
                //payload_length = cur_pkt->data_len;
                //payload = reinterpret_cast<char *>(get_data_pointer(cur_pkt, false));
                //printf("%zuth segment, first byte of payload: %c, 2nd: %c, 2nd to last: %c, last: %c\n", i, payload[0], payload[1], payload[payload_length - 2], payload[payload_length - 1]);
                cur_pkt->pkt_len = lwip_sga->segment_sizes[i];
                if ( i == (lwip_sga->num_segments - 1) ) {
                    cur_pkt->next = NULL;
                }
            }
            pkt = pkts[0];
        } else if (use_external_memory) {
            pkt->pkt_len = total_len;
            //pkt->data_len = total_len;
            pkt->data_len = header_size + sga->sga_segs[0].sgaseg_len;
            pkts[1]->data_len = total_len - pkt->data_len;
            //char *payload = reinterpret_cast<char *>(get_data_pointer(pkts[0], false));
            //size_t payload_length = pkts[0]->data_len;
            //printf("SEND: sga id: %u, 1st segment, first byte of payload: %c, 2nd: %c, 2nd to last: %c, last: %c\n", (unsigned)sga->id, payload[46 + 8], payload[46 + 9], payload[payload_length - 2], payload[payload_length - 1]);
            pkt->nb_segs = 2;
            //pkt->nb_segs = 1;
            //pkt->next = NULL;
            pkt->next = pkts[1];
        } else {
            pkt->data_len = total_len;
            pkt->pkt_len = total_len;
            pkt->nb_segs = 1;
        }

#if DMTR_DEBUG
        printf("send: eth src addr: ");
        DMTR_OK(print_ether_addr(stdout, eth_hdr->s_addr));
        printf("\n");
        printf("send: eth dst addr: ");
        DMTR_OK(print_ether_addr(stdout, eth_hdr->d_addr));
        printf("\n");
        printf("send: ip src addr: %x\n", ntohl(ip_hdr->src_addr));
        printf("send: ip dst addr: %x\n", ntohl(ip_hdr->dst_addr));
        printf("send: udp src port: %d\n", ntohs(udp_hdr->src_port));
        printf("send: udp dst port: %d\n", ntohs(udp_hdr->dst_port));
        printf("send: sga_numsegs: %d\n", sga->sga_numsegs);
        for (size_t i = 0; i < sga->sga_numsegs; ++i) {
            printf("send: buf [%lu] len: %u\n", i, sga->sga_segs[i].sgaseg_len);
        //     printf("send: packet segment [%lu] contents: %s\n", i, reinterpret_cast<char *>(sga->sga_segs[i].sgaseg_buf));
        }
        printf("send: udp len: %d\n", ntohs(udp_hdr->dgram_len));
        printf("send: pkt len: %d\n", total_len);
        rte_pktmbuf_dump(stderr, pkt, total_len);
#endif

        size_t pkts_sent = 0;
#if DMTR_PROFILE
        auto t0 = boost::chrono::steady_clock::now();
        boost::chrono::duration<uint64_t, boost::nano> dt(0);
#endif
        while (pkts_sent < 1) {
            int ret = rte_eth_tx_burst(pkts_sent, dpdk_port_id, 0, &pkt, 1);
            switch (ret) {
                default:
                    DMTR_FAIL(ret);
                case 0:
                    DMTR_TRUE(ENOTSUP, 1 == pkts_sent);
                    q_out_packets++;
                    continue;
                case EAGAIN:
#if DMTR_PROFILE
                    dt += boost::chrono::steady_clock::now() - t0;
#endif
                    yield();
#if DMTR_PROFILE
            t0 = boost::chrono::steady_clock::now();
#endif
                    continue;
            }
        }

#if DMTR_PROFILE
        dt += (boost::chrono::steady_clock::now() - t0);
        DMTR_OK(dmtr_record_latency(write_latency.get(), dt.count()));
#endif

        DMTR_OK(t->complete(0, *sga));
    }

    return 0;
}

int dmtr::lwip_queue::pop(dmtr_qtoken_t qt) {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);
    DMTR_NOTNULL(EINVAL, my_pop_thread);

    DMTR_OK(new_task(qt, DMTR_OPC_POP));
    my_pop_thread->enqueue(qt);

    return 0;
}


int dmtr::lwip_queue::pop_thread(task::thread_type::yield_type &yield, task::thread_type::queue_type &tq) {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);

    while (good()) {
        while (tq.empty()) {
            yield();
        }

        auto qt = tq.front();
        tq.pop();
        task *t;
        DMTR_OK(get_task(t, qt));

        while (my_recv_queue->empty()) {
            if (service_incoming_packets() == EAGAIN ||
                my_recv_queue->empty())
                yield();
        }

        dmtr_sgarray_t &sga = my_recv_queue->front();
        // todo: pop from queue in `raii_guard`.
        DMTR_OK(t->complete(0, sga));
        my_recv_queue->pop();
        q_in_packets++;

    }

    return 0;
}

int
dmtr::lwip_queue::service_incoming_packets() {
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EPERM, our_dpdk_port_id != boost::none);
    const uint16_t dpdk_port_id = boost::get(our_dpdk_port_id);

    // poll DPDK NIC
    struct rte_mbuf *pkts[our_max_queue_depth];
    uint16_t depth = 0;
    DMTR_OK(dmtr_sztou16(&depth, our_max_queue_depth));
    size_t count = 0;
#if DMTR_PROFILE
    auto t0 = boost::chrono::steady_clock::now();
#endif
    int ret = rte_eth_rx_burst(count, dpdk_port_id, 0, pkts, depth);
    switch (ret) {
        default:
            DMTR_FAIL(ret);
        case 0:
            break;
        case EAGAIN:
            return ret;
    }
#if DMTR_PROFILE
    auto dt = boost::chrono::steady_clock::now() - t0;
    DMTR_OK(dmtr_record_latency(read_latency.get(), dt.count()));
#endif
    for (size_t i = 0; i < count; ++i) {
        struct sockaddr_in src, dst;
        dmtr_sgarray_t sga;
        // check the packet header
        bool valid_packet = parse_packet(src, dst, sga, pkts[i]);
#ifdef DMTR_NO_SER
        // this will be freed later
        if (valid_packet) {
            //printf("Setting ptr to dpdk pkt in sga as %p\n", (void *)(pkts[i]));
            sga.recv_segments = (void *)(pkts[i]);
            printf("Received a valid packet\n");
        } else {
            rte_pktmbuf_free(pkts[i]);
        }

#else
        if (zero_copy_mode || use_external_memory) {
            sga.recv_segments = (void *)(pkts[i]);
        } else {
            rte_pktmbuf_free(pkts[i]);
            sga.recv_segments = NULL;
        }
#endif
        if (valid_packet) {
            lwip_addr src_lwip(src);
            // found valid packet, try to place in queue based on src
            if (insert_recv_queue(src_lwip, sga)) {
                // placed in appropriate queue, work is done
#if DMTR_DEBUG
                std::cout << "Found a connected receiver: " << src.sin_addr.s_addr << std::endl;
#endif
                continue;
            }
            // create the new queue
            our_recv_queues[src_lwip] = new std::queue<dmtr_sgarray_t>();
            // put packet into queue
            insert_recv_queue(src_lwip, sga);
            std::cout << "Placing in accept queue: " << src.sin_addr.s_addr << ", has id " << sga.id<< std::endl;
            // also place in accept queue
            insert_recv_queue(lwip_addr(dst), sga);
            in_packets++;
        } else {
            invalid_packets++;
        }
    }
    return 0;
}

bool
dmtr::lwip_queue::parse_packet(struct sockaddr_in &src,
                               struct sockaddr_in &dst,
                               dmtr_sgarray_t &sga,
                               const struct rte_mbuf *pkt)
{
    // packet layout order is (from outside -> in):
    // ether_hdr
    // ipv4_hdr
    // udp_hdr
    // sga.num_bufs
    // sga.buf[0].len
    // sga.buf[0].buf
    // sga.buf[1].len
    // sga.buf[1].buf
    // ...
    auto *p = rte_pktmbuf_mtod(pkt, uint8_t *);
    size_t header = 0;

    // check ethernet header
    auto * const eth_hdr = reinterpret_cast<struct ::rte_ether_hdr *>(p);
    p += sizeof(*eth_hdr);
    header += sizeof(*eth_hdr);
    auto eth_type = ntohs(eth_hdr->ether_type);

#if DMTR_DEBUG
    printf("=====\n");
    printf("recv: pkt len: %d\n", pkt->pkt_len);
    printf("recv: eth src addr: ");
    DMTR_OK(print_ether_addr(stdout, eth_hdr->s_addr));
    printf("\n");
    printf("recv: eth dst addr: ");
    DMTR_OK(print_ether_addr(stdout, eth_hdr->d_addr));
    printf("\n");
    printf("recv: eth type: %x\n", eth_type);
#endif

    struct rte_ether_addr mac_addr = {};

    DMTR_OK(rte_eth_macaddr_get(boost::get(our_dpdk_port_id), mac_addr));
    if (!rte_is_same_ether_addr(&mac_addr, &eth_hdr->d_addr) && !rte_is_same_ether_addr(&ether_broadcast, &eth_hdr->d_addr)) {
#if DMTR_DEBUG
        printf("recv: dropped (wrong eth addr)!\n");
#endif
        return false;
    }

    if (RTE_ETHER_TYPE_IPV4 != eth_type) {
#if DMTR_DEBUG
        printf("recv: dropped (wrong eth type)!\n");
#endif
        return false;
    }

    // check ip header
    auto * const ip_hdr = reinterpret_cast<struct ::rte_ipv4_hdr *>(p);
    p += sizeof(*ip_hdr);
    header += sizeof(*ip_hdr);

    // In network byte order.
    in_addr_t ipv4_src_addr = ip_hdr->src_addr;
    in_addr_t ipv4_dst_addr = ip_hdr->dst_addr;

    if (IPPROTO_UDP != ip_hdr->next_proto_id) {
#if DMTR_DEBUG
        printf("recv: dropped (not UDP)!\n");
#endif
        return false;
    }

#if DMTR_DEBUG
    printf("recv: ip src addr: %x\n", ntohl(ipv4_src_addr));
    printf("recv: ip dst addr: %x\n", ntohl(ipv4_dst_addr));
#endif
    src.sin_addr.s_addr = ipv4_src_addr;
    dst.sin_addr.s_addr = ipv4_dst_addr;

    // check udp header
    auto * const udp_hdr = reinterpret_cast<struct ::rte_udp_hdr *>(p);
    p += sizeof(*udp_hdr);
    header += sizeof(*udp_hdr);

    // In network byte order.
    in_port_t udp_src_port = udp_hdr->src_port;
    in_port_t udp_dst_port = udp_hdr->dst_port;

#if DMTR_DEBUG
    printf("recv: udp src port: %d\n", ntohs(udp_src_port));
    printf("recv: udp dst port: %d\n", ntohs(udp_dst_port));
#endif
    src.sin_port = udp_src_port;
    dst.sin_port = udp_dst_port;
    src.sin_family = AF_INET;
    dst.sin_family = AF_INET;

    // if bound filter out other packets
    if (is_bound()) {
        if (ipv4_dst_addr != my_bound_src->sin_addr.s_addr ||
            udp_dst_port != my_bound_src->sin_port) {
#if DMTR_DEBUG
            printf("dropping because not for my bound address");
#endif
            return false;
        }
    }

    // get the ID
    sga.id = ntohl(*reinterpret_cast<uint32_t *>(p));
    p += sizeof(uint32_t);
    header += sizeof(uint32_t);

#ifdef DMTR_NO_SER
    sga.sga_numsegs = 1;
    // allocate this many headers
#else
    // segment count
    if (zero_copy_mode || use_external_memory) {
        sga.sga_numsegs = 1;
    } else {
        //sga.sga_numsegs = ntohl(*reinterpret_cast<uint32_t *>(p));
        //p += sizeof(uint32_t);
        sga.sga_numsegs = 1;
    }
#endif
#if DMTR_DEBUG
    printf("recv: sga_numsegs: %d\n", sga.sga_numsegs);
#endif
#ifdef DMTR_ALLOCATE_SEGMENTS
    void* segments = malloc(sizeof(dmtr_sgaseg_t *) * sga.sga_numsegs);
    assert(segments != NULL);
    sga.sga_segs = (dmtr_sgaseg_t *)segments;
#endif

#ifdef DMTR_NO_SER
    // actual pointer to dpdk memory
    sga.sga_segs[0].sgaseg_buf = p;
    sga.sga_segs[0].sgaseg_len = pkt->pkt_len - header;
#else
    if (zero_copy_mode || use_external_memory) {
        sga.sga_segs[0].sgaseg_buf = p;
        //printf("Received %zu of data\n", pkt->pkt_len - header);
        sga.sga_segs[0].sgaseg_len = pkt->pkt_len - header;
        // check the bytes that they were actually sent.
        //char *payload = reinterpret_cast<char *>(p) + 8;
        //size_t payload_length = pkt->pkt_len - header - 8;
        //printf("RECVD: pkt:%u, data_len: %u, id: %u\n", pkt->pkt_len, pkt->data_len, (unsigned)sga.id);
        //printf("RECV: addr of ptr: %p\n", payload);
        //printf("---->RECVD: idx: 1, first byte of payload: %c, 2nd: %c, 2nd to last: %c, last: %c\n", payload[0], payload[1], payload[payload_length - 2], payload[payload_length - 1]);
        //for (size_t i = 0; i < lwip_sga->num_segments; i++) {
            //payload_length = lwip_sga->segment_sizes[i];
            //printf("---->RECVD: idx: %zu, first byte of payload: %c, 2nd: %c, 2nd to last: %c, last: %c\n", i, payload[0], payload[1], payload[payload_length - 2], payload[payload_length - 1]);
            //payload += lwip_sga->segment_sizes[i];
        //}

    } else {
        sga.sga_segs[0].sgaseg_len = pkt->pkt_len - header;
        size_t seg_len = sga.sga_segs[0].sgaseg_len;
        void *buf = NULL;
        DMTR_OK(dmtr_malloc(&buf, seg_len));
        rte_memcpy(buf, p, seg_len);
        sga.sga_segs[0].sgaseg_buf = buf;
        /*for (size_t i = 0; i < sga.sga_numsegs; ++i) {
            // segment length
            auto seg_len = ntohl(*reinterpret_cast<uint32_t *>(p));
            sga.sga_segs[i].sgaseg_len = seg_len;
            p += sizeof(seg_len);

#if DMTR_DEBUG
            printf("recv: buf [%lu] len: %u\n", i, seg_len);
#endif
            void *buf = NULL;
            DMTR_OK(dmtr_malloc(&buf, seg_len));
            // printf("Allocating recv buf at %p at length %u\n", (void *)buf, (unsigned)seg_len);
            // for DPDK, pointers are scattered
            sga.sga_buf = NULL;
            sga.sga_segs[i].sgaseg_buf = buf;
            //printf("Allocating sga to: %p, sgasegs is at %p\n", buf, (void *)sga.sga_segs);
            // todo: remove copy if possible.
            // char *s = reinterpret_cast<char *>(p);
            // printf("last char of s: %c\n", s[seg_len - 1]);
            rte_memcpy(buf, p, seg_len);
            p += seg_len;

#if DMTR_DEBUG
            printf("recv: packet segment [%lu] contents: %s\n", i, reinterpret_cast<char *>(buf));
#endif
        }*/
    }
#endif
    sga.sga_addr.sin_family = AF_INET;
    sga.sga_addr.sin_port = udp_src_port;
    sga.sga_addr.sin_addr.s_addr = ipv4_src_addr;
    return true;
}

int dmtr::lwip_queue::poll(dmtr_qresult_t &qr_out, dmtr_qtoken_t qt)
{
    DMTR_OK(task::initialize_result(qr_out, qd(), qt));
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(EINVAL, good());

    task *t;
    if (!has_task(qt)) {
        printf("In lwip queue for %d, cannot find task %lu\n", qd(), qt);
    }
    DMTR_OK(get_task(t, qt));

    int ret;
    switch (t->opcode()) {
    default:
        return ENOTSUP;
    case DMTR_OPC_ACCEPT:
        // run accept once every 10000 times
        static int poll_count = 0;
        poll_count++;
        if (poll_count > 100000) {
            poll_count = 0;
            ret = my_accept_thread->service();
            if (ret != EAGAIN && ret != 0)
                printf("accept problem\n");
        } else ret = EAGAIN;
        break;
    case DMTR_OPC_PUSH: 
    case DMTR_OPC_POP:
        ret = my_pop_thread->service();
        if (ret != EAGAIN && ret != 0) {
            printf("pop problem\n");
            break;
        }
        if (ret == EAGAIN) {
            ret = my_push_thread->service();
            if (ret != EAGAIN && ret != 0)
                printf("push problem\n");
        }
        break;

        
    case DMTR_OPC_CONNECT:
        ret = 0;
        break;
    }

    switch (ret) {
        default:
            DMTR_FAIL(ret);
            exit(-1);
        case EAGAIN:
            break;
        case 0:
            if (DMTR_OPC_CONNECT != t->opcode()) {
                // the threads should only exit if the queue has been closed
                // (`good()` => `false`).
                DMTR_UNREACHABLE();
            }
    }

    return t->poll(qr_out);
}

int dmtr::lwip_queue::rte_eth_macaddr_get(uint16_t port_id, struct rte_ether_addr &mac_addr) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    // todo: how to detect invalid port ids?
    ::rte_eth_macaddr_get(port_id, &mac_addr);
    return 0;
}

int dmtr::lwip_queue::rte_eth_rx_burst(size_t &count_out, uint16_t port_id, uint16_t queue_id, struct rte_mbuf **rx_pkts, const uint16_t nb_pkts) {
    count_out = 0;
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));
    DMTR_NOTNULL(EINVAL, rx_pkts);

    size_t count = ::rte_eth_rx_burst(port_id, queue_id, rx_pkts, nb_pkts);
    if (0 == count) {
        // todo: after enough retries on `0 == count`, the link status
        // needs to be checked to determine if an error occurred.
        return EAGAIN;
    }
    count_out = count;
    return 0;
}

int dmtr::lwip_queue::rte_eth_tx_burst(size_t &count_out, uint16_t port_id, uint16_t queue_id, struct rte_mbuf **tx_pkts, const uint16_t nb_pkts) {
    count_out = 0;
    DMTR_TRUE(EPERM, our_dpdk_init_flag);
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));
    DMTR_NOTNULL(EINVAL, tx_pkts);

    size_t count = ::rte_eth_tx_burst(port_id, queue_id, tx_pkts, nb_pkts);
    // todo: documentation mentions that we're responsible for freeing up `tx_pkts` _sometimes_.
    if (0 == count) {
        // todo: after enough retries on `0 == count`, the link status
        // needs to be checked to determine if an error occurred.
        return EAGAIN;
    }
    count_out = count;
    out_packets += count;
    return 0;
}

int dmtr::lwip_queue::rte_pktmbuf_alloc(struct rte_mbuf *&pkt_out, struct rte_mempool * const mp) {
    pkt_out = NULL;
    DMTR_NOTNULL(EINVAL, mp);
    DMTR_TRUE(EPERM, our_dpdk_init_flag);

    struct rte_mbuf *pkt = ::rte_pktmbuf_alloc(mp);
    DMTR_NOTNULL(ENOMEM, pkt);
    pkt_out = pkt;
    return 0;
}

int dmtr::lwip_queue::rte_pktmbuf_alloc_bulk(struct rte_mbuf** pkts, struct rte_mempool * const mp, size_t num_mbufs) {
    pkts = NULL;
    DMTR_NOTNULL(EINVAL, mp);
    DMTR_TRUE(EPERM, our_dpdk_init_flag);

    DMTR_OK(::rte_pktmbuf_alloc_bulk(mp, pkts, num_mbufs));
    return 0;
}


int dmtr::lwip_queue::rte_eal_init(int &count_out, int argc, char *argv[]) {
    count_out = -1;
    DMTR_NOTNULL(EINVAL, argv);
    DMTR_TRUE(ERANGE, argc >= 0);
    for (int i = 0; i < argc; ++i) {
        DMTR_NOTNULL(EINVAL, argv[i]);
    }

    int ret = ::rte_eal_init(argc, argv);
    if (-1 == ret) {
        return rte_errno;
    }

    if (-1 > ret) {
        DMTR_UNREACHABLE();
    }

    count_out = ret;
    return 0;
}

int dmtr::lwip_queue::rte_pktmbuf_pool_create(struct rte_mempool *&mpool_out, const char *name, unsigned n, unsigned cache_size, uint16_t priv_size, uint16_t data_room_size, int socket_id) {
    mpool_out = NULL;
    DMTR_NOTNULL(EINVAL, name);

    struct rte_mempool *ret = ::rte_pktmbuf_pool_create(name, n, cache_size, priv_size, data_room_size, socket_id);
    if (NULL == ret) {
        return rte_errno;
    }

    mpool_out = ret;
    return 0;
}

int dmtr::lwip_queue::rte_eth_dev_info_get(uint16_t port_id, struct rte_eth_dev_info &dev_info) {
    dev_info = {};
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    ::rte_eth_dev_info_get(port_id, &dev_info);
    return 0;
}

int dmtr::lwip_queue::rte_eth_dev_configure(uint16_t port_id, uint16_t nb_rx_queue, uint16_t nb_tx_queue, const struct rte_eth_conf &eth_conf) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    int ret = ::rte_eth_dev_configure(port_id, nb_rx_queue, nb_tx_queue, &eth_conf);
    // `::rte_eth_dev_configure()` returns device-specific error codes that are supposed to be < 0.
    if (0 >= ret) {
        return ret;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_rx_queue_setup(uint16_t port_id, uint16_t rx_queue_id, uint16_t nb_rx_desc, unsigned int socket_id, const struct rte_eth_rxconf &rx_conf, struct rte_mempool &mb_pool) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    int ret = ::rte_eth_rx_queue_setup(port_id, rx_queue_id, nb_rx_desc, socket_id, &rx_conf, &mb_pool);
    if (0 == ret) {
        return 0;
    }

    if (0 > ret) {
        return 0 - ret;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_tx_queue_setup(uint16_t port_id, uint16_t tx_queue_id, uint16_t nb_tx_desc, unsigned int socket_id, const struct rte_eth_txconf &tx_conf) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    int ret = ::rte_eth_tx_queue_setup(port_id, tx_queue_id, nb_tx_desc, socket_id, &tx_conf);
    if (0 == ret) {
        return 0;
    }

    if (0 > ret) {
        return 0 - ret;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_dev_socket_id(int &sockid_out, uint16_t port_id) {
    sockid_out = 0;

    int ret = ::rte_eth_dev_socket_id(port_id);
    if (-1 == ret) {
        // `port_id` is out of range.
        return ERANGE;
    }

    if (0 <= ret) {
        sockid_out = ret;
        return 0;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_dev_start(uint16_t port_id) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    int ret = ::rte_eth_dev_start(port_id);
    // `::rte_eth_dev_start()` returns device-specific error codes that are supposed to be < 0.
    if (0 >= ret) {
        return ret;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_promiscuous_enable(uint16_t port_id) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    ::rte_eth_promiscuous_enable(port_id);
    return 0;
}

int dmtr::lwip_queue::rte_eth_dev_flow_ctrl_get(uint16_t port_id, struct rte_eth_fc_conf &fc_conf) {
    fc_conf = {};
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    int ret = ::rte_eth_dev_flow_ctrl_get(port_id, &fc_conf);
    if (0 == ret) {
        return 0;
    }

    if (0 > ret) {
        return 0 - ret;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_dev_flow_ctrl_set(uint16_t port_id, const struct rte_eth_fc_conf &fc_conf) {
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    // i don't see a reason why `fc_conf` would be modified.
    int ret = ::rte_eth_dev_flow_ctrl_set(port_id, const_cast<struct rte_eth_fc_conf *>(&fc_conf));
    if (0 == ret) {
        return 0;
    }

    if (0 > ret) {
        return 0 - ret;
    }

    DMTR_UNREACHABLE();
}

int dmtr::lwip_queue::rte_eth_link_get_nowait(uint16_t port_id, struct rte_eth_link &link) {
    link = {};
    DMTR_TRUE(ERANGE, ::rte_eth_dev_is_valid_port(port_id));

    ::rte_eth_link_get_nowait(port_id, &link);
    return 0;
}

int dmtr::lwip_queue::learn_addrs(const struct rte_ether_addr &mac, const struct in_addr &ip) {
    DMTR_TRUE(EINVAL, !rte_is_same_ether_addr(&mac, &ether_broadcast));
    std::string mac_s(reinterpret_cast<const char *>(mac.addr_bytes), RTE_ETHER_ADDR_LEN);
    DMTR_TRUE(EEXIST, our_mac_to_ip_table.find(mac_s) == our_mac_to_ip_table.cend());
    DMTR_TRUE(EEXIST, our_ip_to_mac_table.find(ip.s_addr) == our_ip_to_mac_table.cend());

    our_mac_to_ip_table.insert(std::make_pair(mac_s, ip));
    our_ip_to_mac_table.insert(std::make_pair(ip.s_addr, mac));
    return 0;
}

int dmtr::lwip_queue::learn_addrs(const char *mac_s, const char *ip_s) {
    DMTR_NOTNULL(EINVAL, mac_s);
    DMTR_NOTNULL(EINVAL, ip_s);

    struct rte_ether_addr mac;
    DMTR_OK(parse_ether_addr(mac, mac_s));

    struct in_addr ip = {};
    if (inet_pton(AF_INET, ip_s, &ip) != 1) {
        DMTR_FAIL(EINVAL);
    }

    DMTR_OK(learn_addrs(mac, ip));
    return 0;
}

int dmtr::lwip_queue::parse_ether_addr(struct rte_ether_addr &mac_out, const char *s) {
    static_assert(RTE_ETHER_ADDR_LEN == 6);
    DMTR_NOTNULL(EINVAL, s);

    unsigned int values[RTE_ETHER_ADDR_LEN];
    if (6 != sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%*c", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])) {
        return EINVAL;
    }

    for (size_t i = 0; i < RTE_ETHER_ADDR_LEN; ++i) {
        DMTR_OK(dmtr_utou8(&mac_out.addr_bytes[i], values[i]));
    }

    return 0;
}

int dmtr::lwip_queue::set_zero_copy() {
    zero_copy_mode = true;
    return 0;
}

int dmtr::lwip_queue::set_use_external_memory(void *mmap_addr, uint16_t *mmap_len) {
    DMTR_OK(init_ext_mem(mmap_addr, mmap_len));
    use_external_memory = true;
    // init extbuf mempool
    struct rte_pktmbuf_pool_private mbp_priv;
    unsigned elt_size;
    elt_size = sizeof(struct rte_mbuf) + sizeof(struct rte_pktmbuf_pool_private);
    mbp_priv.mbuf_data_room_size = 0;
    mbp_priv.mbuf_priv_size = 0;
    const uint16_t nb_ports = rte_eth_dev_count_avail();

    extbuf_mbuf_pool = ::rte_mempool_create_empty("extbuf_mempool",
                                        NUM_MBUFS * nb_ports,
                                        elt_size,
                                        0,
                                        sizeof(struct rte_pktmbuf_pool_private),
                                        rte_socket_id(), 0);
    if (extbuf_mbuf_pool == NULL) {
        printf("Unable to initialize extbuf mempool\n");
        return EINVAL;
    }

    rte_pktmbuf_pool_init(extbuf_mbuf_pool, &mbp_priv);
    if (rte_mempool_populate_default(extbuf_mbuf_pool) != (NUM_MBUFS * nb_ports)) {
        printf("Mempool populate didnt init correct number\n");
        return 1;
    }

    if ((unsigned int)(rte_mempool_obj_iter(extbuf_mbuf_pool, rte_pktmbuf_init, NULL)) != 
            (unsigned int)(NUM_MBUFS * nb_ports)) {
                printf("Mempool obj iter didn't init correct number.\n");
                return 1;
        };

    return 0;
}

int dmtr::lwip_queue::init_mempools(uint32_t message_size, uint32_t num_segments) {

    // initialize the lwip sga
    lwip_sga->num_segments = num_segments;
    uint32_t segment_size = message_size / num_segments;
    uint32_t remaining = (message_size % num_segments);
    uint32_t last_segment_size = segment_size + remaining;
    lwip_sga->total_size = message_size;
    for (uint32_t i = 0; i < num_segments; i++) {
        lwip_sga->segment_sizes[i] = segment_size;
        if (i < remaining) {
            lwip_sga->segment_sizes[i] += 1; // have the larger number of segments
        }
    }

    /*for (uint32_t i = 0; i < num_segments; i++) {
        printf("Segment %zu size: %zu\n", i, lwip_sga->segment_sizes[i]);
    }*/

    const uint16_t nb_ports = rte_eth_dev_count_avail();
    DMTR_TRUE(ENOENT, nb_ports > 0);
    fprintf(stderr, "DPDK reports that %d ports (interfaces) are available.\n", nb_ports);
    struct rte_mempool *mbuf_pool = NULL;
    // for header structs
    DMTR_OK(rte_pktmbuf_pool_create(
                                    mbuf_pool,
                                    "header_mbuf_pool",
                                    NUM_MBUFS * nb_ports,
                                    MBUF_CACHE_SIZE,
                                    0,
                                    MBUF_BUF_SIZE,
                                    rte_socket_id()));
    header_mbuf_pool = mbuf_pool;
    mbuf_pool = NULL;

    // for struct of the segment size
    DMTR_OK(rte_pktmbuf_pool_create(
                                    mbuf_pool,
                                    "payload_mbuf_pool1",
                                    NUM_MBUFS * nb_ports,
                                    MBUF_CACHE_SIZE,
                                    0,
                                    MBUF_BUF_SIZE,
                                    rte_socket_id()));
    payload_mbuf_pool1 = mbuf_pool;
    mbuf_pool = NULL;
   
    // if last segment in scatter has an unequal segment size    
    DMTR_OK(rte_pktmbuf_pool_create(
                                    mbuf_pool,
                                    "payload_mbuf_pool2",
                                    NUM_MBUFS * nb_ports,
                                    MBUF_CACHE_SIZE,
                                    0,
                                    MBUF_BUF_SIZE,
                                    rte_socket_id()));
    payload_mbuf_pool2 = mbuf_pool;
    mbuf_pool = NULL;
    dmtr::lwip_queue::current_segment_size = segment_size;
    for (int i = 0; i < 3; i++) {
        switch (i) {
        default:
            return EINVAL;
        case 0:
            mbuf_pool = header_mbuf_pool;
            break;
        case 1:
            mbuf_pool = payload_mbuf_pool1;
            break;
        case 2:
            mbuf_pool = payload_mbuf_pool2;
            dmtr::lwip_queue::current_segment_size = last_segment_size;
            break;
        }
        dmtr::lwip_queue::current_has_header = (i == 0) ? true : false;
        
        rte_mempool_obj_iter(mbuf_pool, &custom_init, NULL);
    }
    return 0;
}

void dmtr::lwip_queue::custom_init(struct rte_mempool *mp, void *opaque_arg, void *m, unsigned i) {
    struct rte_mbuf* pkt = reinterpret_cast<struct rte_mbuf *>(m);
    auto *p = rte_pktmbuf_mtod(pkt, uint8_t *);
    if (dmtr::lwip_queue::current_has_header) {
        auto * const eth_hdr = reinterpret_cast<struct ::rte_ether_hdr *>(p);
        p += sizeof(*eth_hdr);
        auto * const ip_hdr = reinterpret_cast<struct ::rte_ipv4_hdr *>(p);
        p += sizeof(*ip_hdr);
        auto * const udp_hdr = reinterpret_cast<struct ::rte_udp_hdr *>(p);
        p += sizeof(*udp_hdr);
        p += sizeof(uint32_t);
    }
    char *s = reinterpret_cast<char *>(p);
    memset(s, FILL_CHAR, dmtr::lwip_queue::current_segment_size);
    s[0] = 'd';
    s[dmtr::lwip_queue::current_segment_size - 1] = 'b';
}


void * dmtr::lwip_queue::get_data_pointer(struct rte_mbuf * pkt, bool has_header) {
    auto *p = rte_pktmbuf_mtod(pkt, uint8_t *);
    if (has_header) {
        auto * const eth_hdr = reinterpret_cast<struct ::rte_ether_hdr *>(p);
        p += sizeof(*eth_hdr);
        auto * const ip_hdr = reinterpret_cast<struct ::rte_ipv4_hdr *>(p);
        p += sizeof(*ip_hdr);
        auto * const udp_hdr = reinterpret_cast<struct ::rte_udp_hdr *>(p);
        p += sizeof(*udp_hdr);
        p += sizeof(uint32_t);
    }
    return (void *)p;
}

size_t dmtr::lwip_queue::get_header_size() {
    void *p = NULL;
    size_t header_size;
    auto * const eth_hdr = reinterpret_cast<struct ::rte_ether_hdr *>(p);
    header_size += sizeof(*eth_hdr);
    auto * const ip_hdr = reinterpret_cast<struct ::rte_ipv4_hdr *>(p);
    header_size += sizeof(*ip_hdr);
    auto * const udp_hdr = reinterpret_cast<struct ::rte_udp_hdr *>(p);
    header_size += sizeof(*udp_hdr);
    header_size += sizeof(uint32_t);
    return header_size;
}

void dmtr::lwip_queue::start_threads() {
    if (my_listening_flag) {
        my_accept_thread.reset(new task::thread_type([=](task::thread_type::yield_type &yield, task::thread_type::queue_type &tq) {
            return accept_thread(yield, tq);
        }));
    } else {
        my_push_thread.reset(new task::thread_type([=](task::thread_type::yield_type &yield, task::thread_type::queue_type &tq) {
            return push_thread(yield, tq);
        }));

        my_pop_thread.reset(new task::thread_type([=](task::thread_type::yield_type &yield, task::thread_type::queue_type &tq) {
            return pop_thread(yield, tq);
        }));
    }
}
