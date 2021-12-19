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
    : client_socket_(std::move(socket)),
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
    cout << length << endl;
    for (; i < length; ++i) {
      printf("%02x ", data[i]);
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

          cout << length << endl;

          debug_dump(data_, length);

          // Parse SOCKS4_REQUEST
          if (length < 9) {
            cout << "[!] Unexpected SOCKS4_REQUEST: Length error" << endl;
            return;
          }

          vn = data_[0];
          cd_ = data_[1];

          // Big endian
          data_[2] ^= data_[3];
          data_[3] ^= data_[2];
          data_[2] ^= data_[3];
          dstport = *((WORD *)&data_[2]);
          dstip = *((DWORD *)&data_[4]);          
          userid = &data_[8];
          
          sprintf(s_port, "%d", dstport);

          if (cd_ == 1) {
            // CONNECT

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
          } else if (cd_ == 2) {
            // BIND
          }
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

          do_connect();
        } else {
          cout << "[!] Resolve failed" << endl;
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

          if (cd_ == 1) {
            // CONNECT
            do_SOCKS4_reply(1);
          } else if (cd_ == 2) {
            // BIND
          }
        } else {
          cout << "[!] Connect failed (" << server_endpoint_ << ")" << endl;
          do_SOCKS4_reply(0);
        }
      });
  }

  void do_SOCKS4_reply(int ok) {
    auto self(shared_from_this());

    SOCKS4_REPLY reply;

    reply.vn = 0;
    reply.cd = ok ? 90 : 91;
    reply.dstport = 0;
    reply.dstip = 0;

    debug_dump((char *)&reply, sizeof(reply));

    boost::asio::async_write(client_socket_, boost::asio::buffer((char *)&reply, sizeof(reply)),
      [this, self](boost::system::error_code ec, std::size_t /*length*/) {
        if (!ec) {
          cout << "[O] Reply OK (" << server_endpoint_ << ")" << endl;
          do_client_read();
          do_server_read();
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

  tcp::socket client_socket_;
  tcp::socket server_socket_;
  enum { max_length = 1024 };
  char data_[max_length];
  char data2_[max_length];
  BYTE cd_;
  tcp::resolver resolver_;
  tcp::endpoint server_endpoint_;
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