//
// async_tcp_echo_server.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2021 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unordered_map>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <boost/asio.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio/signal_set.hpp>

using boost::asio::ip::tcp;
using namespace std;

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;

struct SOCKS4_REPLY {
  BYTE vn;
  BYTE cd;
  WORD dstport;
  DWORD dstip;
};

class session
  : public std::enable_shared_from_this<session>
{
public:
  session(tcp::socket socket, boost::asio::io_context& io_context)
    : io_context_(io_context),
      client_socket_(std::move(socket)),
      server_socket_(io_context),
      resolver_(boost::asio::make_strand(io_context))
  {
  }

  void start()
  {
    do_handle_SOCKS4_request();
  }

private:
  void debug_dump(char *data, int length) {
    int cnt = 0;
    int i = 0;
    cout << "[debug] Length: " << length << endl;
    for (; i < length; ++i) {
      printf("%02x ", (BYTE)data[i]);
      cnt += 1;
      if (cnt == 0x10) {
        printf(" | ");
        for (; cnt; cnt--) {
          if (32 <= data[i + 1 - cnt] && data[i + 1 - cnt] <= 127) {
            printf("%c ", data[i + 1 - cnt]);
          } else {
            printf(". ");
          }
        }
        cout << endl;
      }
    }
    if (length % 0x10) {
      for (int j = 0x10 - (length % 0x10); j; --j) {
        printf("-- ");
      }
      printf(" | ");
    }
    for (; cnt; cnt--) {
      if (32 <= data[i - cnt] && data[i - cnt] <= 127) {
        printf("%c ", data[i - cnt]);
      } else {
        printf(". ");
      }
    }
    cout << endl;
  }

  WORD int_to_port(int port) {
    return ((port & 0xff00) >> 8) | ((port & 0xff) << 8);
  }

  DWORD ip_to_dword(string ip) {
    struct sockaddr_in sa;
    
    inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr));

    return sa.sin_addr.s_addr;
  }

  void do_handle_SOCKS4_request()
  {
    auto self(shared_from_this());
    client_socket_.async_read_some(boost::asio::buffer(data_, max_length),
      [this, self](boost::system::error_code ec, std::size_t length)
      {
        if (!ec) {
          BYTE vn;
          WORD dstport;
          DWORD dstip;
          char *userid;
          char *domain_name;
          char s_port[0x8] = { 0 };

          debug_dump(data_, length);

          // Parse SOCKS4_REQUEST
          if (length < 9) {
            cout << "[!] Unexpected SOCKS4_REQUEST: Length error" << endl;
            return;
          }

          vn = data_[0];

          if (vn != 4) {
            cout << "[!] Unexpected SOCKS4_REQUEST: VN error" << endl;
            return;
          }

          cd_ = data_[1];

          // Big endian
          data_[2] ^= data_[3];
          data_[3] ^= data_[2];
          data_[2] ^= data_[3];
          dstport = *((WORD *)&data_[2]);
          dstip = *((DWORD *)&data_[4]);          
          userid = &data_[8];
          
          sprintf(s_port, "%d", dstport);

          // Recognize SOCKS4/4A
          if ((dstip & 0x00ffffff) == 0) {
            cout << "[*] SOCKS4A request" << endl;

            int idx = 8 + strlen(userid) + 1;

            if (idx >= (int)length) {
              cout << "[!] Unexpected SOCKS4A_REQUEST: USERID error" << endl;
              return;
            }

            domain_name = &data_[idx];

            cout << domain_name << ":" << s_port << endl;

            do_resolve(domain_name, s_port);
          } else {
            cout << "[*] SOCKS4  request" << endl;

            // Turn dstip from int to IP string (xxx.xxx.xxx.xxx)
            char s_dstip[0x10] = { 0 };

            sprintf(s_dstip, "%d.%d.%d.%d", 
                    (dstip) & 0xff,
                    (dstip >> 0x8) & 0xff,
                    (dstip >> 0x10) & 0xff,
                    (dstip >> 0x18) & 0xff);

            cout << s_dstip << ":" << s_port << endl;

            do_resolve(s_dstip, s_port);
          }
        }
      });
  }

  void do_bind()
  {
    auto self(shared_from_this());

    int port = 0x5566;
    DWORD proxy_ip;

    reply_cnt_ = 0;

    proxy_ip = ip_to_dword(client_socket_.local_endpoint().address().to_string());

    while (true) {
      try {
        p_acceptor_ = new tcp::acceptor(io_context_, tcp::endpoint(tcp::v4(), port));
        break;
      } 
      catch (std::exception& e)
      {
        port += 1;
      }
    }

    // Reply client which port to use
    do_SOCKS4_reply(1, int_to_port(port), proxy_ip);

    p_acceptor_->async_accept(
      [this, self, port, proxy_ip](boost::system::error_code ec, tcp::socket socket)
      {
        if (!ec) {
          // Verify the incoming end point is what it should be
          if (server_endpoint_.address().to_string() != socket.remote_endpoint().address().to_string()) {
            cout << "[X] BIND - Other server connected (" << socket.remote_endpoint() << ")" << endl;
            return;
          }
          
          cout << "[O] BIND - Server connected (" << server_endpoint_ << ")" << endl;
          
          server_socket_ = tcp::socket(std::move(socket));

          // Ok, send reply to client
          // Start proxing data from server to client
          do_SOCKS4_reply(1, int_to_port(port), proxy_ip);
        } else {
          cout << "[x] BIND Accept error: " << ec << endl;
        }
      });
  }

  void do_resolve(string hostname, string port)
  {
    auto self(shared_from_this());
    resolver_.async_resolve(
      string(hostname),
      string(port),
      [this, self](boost::system::error_code ec, tcp::resolver::results_type endpoints)
      {
        if (!ec) {
          for (auto it = endpoints.cbegin(); it != endpoints.cend(); it++) {
            server_endpoint_ = *it;
            break;
          }

          cout << "[O] Resolve OK (" << server_endpoint_ << ")" << endl;

          if (cd_ == 1) {
            // CONNECT
            do_connect();
          } else if (cd_ == 2) {
            // BIND
            do_bind();
          }
        } else {
          cout << "[!] Resolve failed" << endl;
          do_SOCKS4_reply(0, 0, 0);
        }
      });
  }

  void do_connect()
  {
    auto self(shared_from_this());
    server_socket_.async_connect(
      server_endpoint_,
      [this, self](boost::system::error_code ec)
      {
        if (!ec) {
          cout << "[O] Connect OK (" << server_endpoint_ << ")" << endl;
          do_SOCKS4_reply(1, 0, 0);
        } else {
          cout << "[!] Connect failed (" << server_endpoint_ << ")" << endl;
          do_SOCKS4_reply(0, 0, 0);
        }
      });
  }

  void do_SOCKS4_reply(int ok, WORD dstport, DWORD dstip) {
    auto self(shared_from_this());

    SOCKS4_REPLY reply;

    reply.vn = 0;
    reply.cd = ok ? 90 : 91;
    reply.dstport = dstport;
    reply.dstip = dstip;

    debug_dump((char *)&reply, sizeof(reply));

    boost::asio::async_write(client_socket_, boost::asio::buffer((char *)&reply, sizeof(reply)),
      [this, self, ok](boost::system::error_code ec, std::size_t /*length*/) {
        if (!ec) {
          cout << "[O] Reply OK (" << server_endpoint_ << ")" << endl;
          
          if (ok) {
            if (cd_ == 1) {
              // CONNECT
              do_client_read();
              do_server_read();
            } else if (cd_ == 2) {
              // BIND
              ++reply_cnt_;

              if (reply_cnt_ == 2) {
                do_client_read();
                do_server_read();
              }
            }
          }
        } else {
          cout << "[!] Reply failed (" << server_endpoint_ << ")" << endl;
        }
      });
  }

  void do_client_read() {
    auto self(shared_from_this());
    client_socket_.async_read_some(boost::asio::buffer(data_, max_length),
      [this, self](boost::system::error_code ec, std::size_t length)
      {
        if (!ec) {
          // debug_dump(data_, length);
          do_server_write(length);
        } else {
          cout << "[!] (Client) Read failed" << endl;
          server_socket_.close();
        }
      });
  }

  void do_client_write(int length) {
    auto self(shared_from_this());

    boost::asio::async_write(client_socket_, boost::asio::buffer(data2_, length),
      [this, self](boost::system::error_code ec, std::size_t /*length*/) {
        if (!ec) {
          cout << "[O] (Client) Write OK (" << server_endpoint_ << ")" << endl;
        } else {
          cout << "[!] (Client) Write failed (" << server_endpoint_ << ")" << endl;
        }
        do_server_read();
      });
  }

  void do_server_read() {
    auto self(shared_from_this());
    server_socket_.async_read_some(boost::asio::buffer(data2_, max_length),
      [this, self](boost::system::error_code ec, std::size_t length)
      {
        if (!ec) {
          // debug_dump(data2_, length);
          do_client_write(length);
        } else {
          cout << "[!] (Server) Read failed" << server_endpoint_ << endl;
          client_socket_.close();
        }
      });
  }

  void do_server_write(int length) {
    auto self(shared_from_this());

    boost::asio::async_write(server_socket_, boost::asio::buffer(data_, length),
      [this, self](boost::system::error_code ec, std::size_t /*length*/) {
        if (!ec) {
          cout << "[O] (Server) Write OK (" << server_endpoint_ << ")" << endl;
        } else {
          cout << "[!] (Server) Write failed (" << server_endpoint_ << ")" << endl;
        }
        do_client_read();
      });
  }

  boost::asio::io_context& io_context_;
  tcp::socket client_socket_;
  tcp::socket server_socket_;
  enum { max_length = 1024 };
  char data_[max_length];
  char data2_[max_length];
  BYTE cd_;
  BYTE reply_cnt_;
  tcp::resolver resolver_;
  tcp::endpoint server_endpoint_;
  tcp::acceptor *p_acceptor_;
};

class server
{
public:
  server(boost::asio::io_context& io_context, short port)
    : io_context_(io_context),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
      signal_(io_context, SIGCHLD)
  {
    wait_for_signal();
    do_accept();
  }

private:
  void wait_for_signal()
  {
    signal_.async_wait(
      [this](boost::system::error_code /*ec*/, int /*signo*/)
      {
        if (acceptor_.is_open()) {
          int status = 0;
          while (waitpid(-1, &status, WNOHANG) > 0) {}

          wait_for_signal();
        }
      });
  }

  void do_accept()
  {
    acceptor_.async_accept(
      [this](boost::system::error_code ec, tcp::socket socket)
      {
        if (!ec) {
          pid_t pid;

          io_context_.notify_fork(boost::asio::io_context::fork_prepare);

          if ((pid = fork())) {
            // Parent
            io_context_.notify_fork(boost::asio::io_context::fork_parent);
            socket.close();
            do_accept();
          } else if (pid == 0) {
            // Child
            io_context_.notify_fork(boost::asio::io_context::fork_child);
            signal_.cancel();
            acceptor_.close();
            std::make_shared<session>(std::move(socket), io_context_)->start();
          } else {
            // Error
            cout << "[x] Fork error" << endl;
            exit(1);
          }
        } else {
          cout << "[x] Accept error" << endl;
          do_accept();
        }
      });
  }

  boost::asio::io_context& io_context_;
  tcp::acceptor acceptor_;
  boost::asio::signal_set signal_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 2) {
      std::cout << "Usage: socks_server <port>\n";
      return 1;
    }

    boost::asio::io_context io_context;

    server s(io_context, std::atoi(argv[1]));

    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cout << "Exception: " << e.what() << "\n";
  }

  return 0;
}