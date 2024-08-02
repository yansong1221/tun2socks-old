#pragma once
#include "interface.hpp"
#include "ip_packet.hpp"
#include "route/route.hpp"
#include "tcp_packet.hpp"
#include "tcp_proxy.hpp"
#include "tuntap/tuntap.hpp"
#include "udp_packet.hpp"
#include "udp_proxy.hpp"
#include <queue>
#include <boost/circular_buffer.hpp>
class ip_layer_stack : public abstract::tun2socks
{
public:
    explicit ip_layer_stack(boost::asio::io_context &ioc)
        : ioc_(ioc)
        , tuntap_(ioc)
    {}
    void start()
    {
        tuntap::tun_parameter param;
        param.tun_name = "mate";

        tuntap::tun_parameter::address tun_ipv4;
        tun_ipv4.addr = boost::asio::ip::address_v4::from_string("10.6.7.7");
        tun_ipv4.dns = boost::asio::ip::address_v4::from_string("114.114.114.114");
        tun_ipv4.prefix_length = 24;
        param.ipv4 = tun_ipv4;

        tuntap::tun_parameter::address tun_ipv6;
        tun_ipv6.addr = boost::asio::ip::address_v6::from_string("fe80::613b:4e3f:81e9:7e01");
        tun_ipv6.dns = boost::asio::ip::address_v6::from_string("2606:4700:4700::1111");
        tun_ipv6.prefix_length = 64;
        param.ipv6 = tun_ipv6;

        tuntap_.open(param);

        auto ipv4_route = route::get_default_ipv4_route();
        auto ipv6_route = route::get_default_ipv6_route();

        if (ipv4_route)
            default_if_addr_v4_ = ipv4_route->if_addr;
        if (ipv6_route)
            default_if_addr_v6_ = ipv6_route->if_addr;

        spdlog::info("默认网络出口v4: {0}", default_if_addr_v4_.to_string());
        spdlog::info("默认网络出口v6: {0}", default_if_addr_v6_.to_string());

        {
            route::route_ipv4 info;
            info.if_addr = tun_ipv4.addr.to_v4();
            info.metric = 0;
            info.netmask = boost::asio::ip::address_v4::any();
            info.network = boost::asio::ip::address_v4::any();
            route::add_route_ipapi(info);
        }
        {
            route::route_ipv6 info;
            info.if_addr = tun_ipv6.addr.to_v6();
            info.metric = 1;
            info.dest = boost::asio::ip::address_v6::any();
            info.prefix_length = 0;
            route::add_route_ipapi(info);
        }

        boost::asio::co_spawn(ioc_, receive_ip_packet(), boost::asio::detached);
    }

    void on_ip_packet(tuntap::recv_ref_buffer buffer)
    {
        auto ip_pack = network_layer::recv_ip_packet::from_recv_buffer(buffer);
        if (!ip_pack)
            return;

        switch (ip_pack->next_protocol()) {
        case transport_layer::recv_udp_packet::protocol:
            on_udp_packet(*ip_pack);
            break;
        case transport_layer::recv_tcp_packet::protocol:
            on_tcp_packet(*ip_pack);
            break;
        default:
            break;
        }
    }
    boost::asio::awaitable<void> receive_ip_packet()
    {
        for (;;) {
            boost::system::error_code ec;
            auto buffer = co_await tuntap_.async_read_some(ec);
            if (ec)
                co_return;
            this->on_ip_packet(buffer);
        }
    };

    void close_endpoint_pair(const transport_layer::tcp_endpoint_pair &endpoint_pair) override
    {
        tcp_proxy_map_.erase(endpoint_pair);
    }
    void close_endpoint_pair(const transport_layer::udp_endpoint_pair &endpoint_pair) override
    {
        udp_proxy_map_.erase(endpoint_pair);
    }

    void write_tun_packet(const transport_layer::send_tcp_packet &pack) override
    {
        auto payload = std::make_shared<boost::asio::streambuf>();
        pack.make_packet(payload->prepare(pack.raw_packet_size()));
        payload->commit(pack.raw_packet_size());

        network_layer::send_ip_packet ip_pack(pack.endpoint_pair().to_address_pair(),
                                              transport_layer::send_tcp_packet::protocol,
                                              tuntap::send_ref_buffer(payload));
        write_ip_packet(ip_pack);
    }
    void write_tun_packet(const transport_layer::send_udp_packet &pack) override
    {
        auto payload = std::make_shared<boost::asio::streambuf>();
        pack.make_packet(payload->prepare(pack.raw_packet_size()));
        payload->commit(pack.raw_packet_size());

        network_layer::send_ip_packet ip_pack(pack.endpoint_pair().to_address_pair(),
                                              transport_layer::send_udp_packet::protocol,
                                              tuntap::send_ref_buffer(payload));
        write_ip_packet(ip_pack);
    }
    virtual boost::asio::awaitable<tcp_socket_ptr> create_proxy_socket(
        const transport_layer::tcp_endpoint_pair &endpoint_pair)
    {
        spdlog::info("tcp proxy: {}", endpoint_pair.to_string());

        auto pid = local_port_pid::tcp_using_port(endpoint_pair.src.port());
        local_port_pid::PrintProcessInfo(pid);

        boost::system::error_code ec;

        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(
            co_await boost::asio::this_coro::executor);

        if (true) {
            open_bind_socket(*socket, endpoint_pair.dest, ec);
            if (ec)
                co_return nullptr;

            co_await socket->async_connect(endpoint_pair.dest, net_awaitable[ec]);
            if (ec) {
                spdlog::warn("can't connect remote endpoint [{0}]:{1}",
                             endpoint_pair.dest.address().to_string(),
                             endpoint_pair.dest.port());
                co_return nullptr;
            }

        } else {
            open_bind_socket(*socket, socks5_endpoint_, ec);
            if (ec)
                co_return nullptr;

            co_await socket->async_connect(socks5_endpoint_, net_awaitable[ec]);

            if (ec) {
                spdlog::warn("can't connect socks5 server [{0}]:{1}",
                             socks5_endpoint_.address().to_string(),
                             socks5_endpoint_.port());
                co_return nullptr;
            }
            proxy::socks_client_option op;
            op.target_host = endpoint_pair.dest.address().to_string();
            op.target_port = endpoint_pair.dest.port();
            op.proxy_hostname = false;

            boost::asio::ip::tcp::endpoint remote_endp;
            co_await proxy::async_socks_handshake(*socket, op, remote_endp, ec);
            if (ec) {
                spdlog::warn("can't connect socks5 server [{0}]:{1}",
                             socks5_endpoint_.address().to_string(),
                             socks5_endpoint_.port());
                co_return nullptr;
            }
        }
        co_return socket;
    }
    boost::asio::awaitable<udp_socket_ptr> create_proxy_socket(
        const transport_layer::udp_endpoint_pair &endpoint_pair,
        boost::asio::ip::udp::endpoint &proxy_endpoint) override
    {
        spdlog::info("udp proxy: {}", endpoint_pair.to_string());

        auto pid = local_port_pid::udp_using_port(endpoint_pair.src.port());
        local_port_pid::PrintProcessInfo(pid);

        if (endpoint_pair.dest.protocol() == boost::asio::ip::udp::v6())
            co_return nullptr;

        boost::system::error_code ec;

        auto socket = std::make_shared<boost::asio::ip::udp::socket>(
            co_await boost::asio::this_coro::executor);

        if (true) {
            open_bind_socket(*socket, endpoint_pair.dest, ec);
            if (ec)
                co_return nullptr;
            proxy_endpoint = endpoint_pair.dest;
        } else {
            boost::asio::ip::tcp::socket proxy_sock(co_await boost::asio::this_coro::executor);
            open_bind_socket(proxy_sock, socks5_endpoint_, ec);
            if (ec)
                co_return nullptr;

            co_await proxy_sock.async_connect(socks5_endpoint_, net_awaitable[ec]);

            if (ec) {
                spdlog::warn("can't connect socks5 server [{0}]:{1}",
                             socks5_endpoint_.address().to_string(),
                             socks5_endpoint_.port());
                co_return nullptr;
            }
            proxy::socks_client_option op;
            op.target_host = endpoint_pair.dest.address().to_string();
            op.target_port = endpoint_pair.dest.port();
            op.proxy_hostname = false;

            boost::asio::ip::udp::endpoint remote_endp;
            co_await proxy::async_socks_handshake(proxy_sock, op, remote_endp, ec);
            if (ec) {
                spdlog::warn("can't connect socks5 server [{0}]:{1}",
                             socks5_endpoint_.address().to_string(),
                             socks5_endpoint_.port());
                co_return nullptr;
            }
            open_bind_socket(*socket, proxy_endpoint, ec);
            if (ec)
                co_return nullptr;

            proxy_endpoint = remote_endp;
        }
        co_return socket;
    }
    void write_ip_packet(const network_layer::send_ip_packet &ip_pack)
    {
        auto buffer = std::make_shared<boost::asio::streambuf>();
        ip_pack.make_packet(buffer->prepare(ip_pack.raw_packet_size()));
        buffer->commit(ip_pack.raw_packet_size());

        tuntap_.write_packet(tuntap::send_ref_buffer(buffer));
    }

private:
    template<typename Stream, typename InternetProtocol>
    inline void open_bind_socket(Stream &sock,
                                 const boost::asio::ip::basic_endpoint<InternetProtocol> &dest,
                                 boost::system::error_code &ec)
    {
        sock.open(dest.protocol());
        if (dest.protocol() == InternetProtocol::v4())
            sock.bind(boost::asio::ip::basic_endpoint<InternetProtocol>(default_if_addr_v4_, 0), ec);
        else
            sock.bind(boost::asio::ip::basic_endpoint<InternetProtocol>(default_if_addr_v6_, 0), ec);
        if (ec)
            spdlog::error("bind {0}", ec.message());
    }
    void on_udp_packet(const network_layer::recv_ip_packet &ip_pack)
    {
        auto udp_pack = transport_layer::recv_udp_packet::from_ip_packet(ip_pack);
        if (!udp_pack)
            return;

        auto endpoint_pair = udp_pack->endpoint_pair();
        auto proxy = udp_proxy_map_[endpoint_pair];
        if (!proxy) {
            proxy = std::make_shared<udp_proxy>(ioc_, endpoint_pair, *this);
            proxy->start();
            udp_proxy_map_[endpoint_pair] = proxy;
        }
        proxy->on_udp_packet(*udp_pack);
    }
    void on_tcp_packet(const network_layer::recv_ip_packet &ip_pack)
    {
        auto tcp_pack = transport_layer::recv_tcp_packet::from_ip_packet(ip_pack);
        if (!tcp_pack)
            return;

        auto endpoint_pair = tcp_pack->endpoint_pair();

        auto proxy = tcp_proxy_map_[endpoint_pair];
        if (!proxy) {
            proxy = std::make_shared<transport_layer::tcp_proxy>(ioc_, endpoint_pair, *this);
            tcp_proxy_map_[endpoint_pair] = proxy;
        }
        proxy->on_tcp_packet(*tcp_pack);
    }

private:
    boost::asio::io_context &ioc_;
    tuntap::tuntap tuntap_;

    boost::asio::ip::address_v4 default_if_addr_v4_;
    boost::asio::ip::address_v6 default_if_addr_v6_;

    boost::asio::ip::tcp::endpoint socks5_endpoint_;

    std::unordered_map<transport_layer::tcp_endpoint_pair, transport_layer::tcp_proxy::ptr>
        tcp_proxy_map_;
    std::unordered_map<transport_layer::udp_endpoint_pair, udp_proxy::ptr> udp_proxy_map_;
};