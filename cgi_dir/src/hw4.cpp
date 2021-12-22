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
#include <fstream>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <boost/asio.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string.hpp>

using tcp = boost::asio::ip::tcp;
using namespace std;

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;

static DWORD ip_to_dword(string ip) {
  struct sockaddr_in sa;
  
  inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr));
  return sa.sin_addr.s_addr;
}

static WORD int_to_port(int port) {
  return ((port & 0xff00) >> 8) | ((port & 0xff) << 8);
}

struct SOCKS4_REQUEST {
  SOCKS4_REQUEST() {
  }

  SOCKS4_REQUEST(tcp::endpoint endpoint) {
    // CONNECT
    vn = 4;
    cd = 1;
    dstport = int_to_port(endpoint.port());
    dstip = ip_to_dword(endpoint.address().to_string());
    nullb = 0;
  }

  BYTE vn;
  BYTE cd;
  WORD dstport;
  DWORD dstip;
  BYTE nullb;
};

struct SOCKS4_REPLY {
  SOCKS4_REPLY() {
  }

  SOCKS4_REPLY(char *data) {
    vn = data[0];
    cd = data[1];
    dstport = int_to_port(*((WORD *)&data[2]));
    dstip = *((DWORD *)&data[4]);
  }

  BYTE vn;
  BYTE cd;
  WORD dstport;
  DWORD dstip;
};

struct socks_info {
  socks_info() {
    enable = 0;
  }
  
  int enable;
  string hostname;
  string port;
};

struct connect_info {
  connect_info() {
    hostname = "";
  }

  string server;
  string hostname;
  string port;
  string testcasename;
};

class client
  : public std::enable_shared_from_this<client>
{
public:
  client(boost::asio::io_context& io_context, connect_info info, socks_info socks_setting)
    : resolver_(boost::asio::make_strand(io_context)),
      socket_(io_context)
  {
    info_ = info;
    socks_setting_ = socks_setting;
    
    // Read testcase
    string filename = "./test_case/" + info_.testcasename;
    cerr << "[T] testcase filename: " << filename << endl;
    ifstream testcasefile (filename);

    if (testcasefile.is_open()) {
      string line;
      while (getline(testcasefile, line)) {
        cerr << "[T] testcase: " << line << endl;
        testcase_.push_back(line + "\n");
      }
      testcasefile.close();
    }

    cerr << "[^] Constructor (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;
    if (socks_setting_.enable == 1) {
      cerr << "[^]\tuse SOCKS (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
    }
  }

  void start()
  {
    do_resolve();
  }

private:
  void debug_dump(char *data, int length) {
    int cnt = 0;
    int i = 0;
    cerr << "[debug] Length: " << length << endl;
    for (; i < length; ++i) {
      fprintf(stderr, "%02x ", (BYTE)data[i]);
      cnt += 1;
      if (cnt == 0x10) {
        fprintf(stderr, " | ");
        for (; cnt; cnt--) {
          if (32 <= data[i + 1 - cnt] && data[i + 1 - cnt] <= 127) {
            fprintf(stderr, "%c ", data[i + 1 - cnt]);
          } else {
            fprintf(stderr, ". ");
          }
        }
        cerr << endl;
      }
    }
    if (length % 0x10) {
      for (int j = 0x10 - (length % 0x10); j; --j) {
        fprintf(stderr, "-- ");
      }
      fprintf(stderr, " | ");
    }
    for (; cnt; cnt--) {
      if (32 <= data[i - cnt] && data[i - cnt] <= 127) {
        fprintf(stderr, "%c ", data[i - cnt]);
      } else {
        fprintf(stderr, ". ");
      }
    }
    cerr << endl;
  }

  void do_resolve()
  {
    auto self(shared_from_this());

    resolver_.async_resolve(
      string(info_.hostname),
      string(info_.port),
      [this, self](boost::system::error_code ec, tcp::resolver::results_type endpoints)
      {
        if (!ec) {
          for (auto it = endpoints.cbegin(); it != endpoints.cend(); it++) {
            endpoint_ = *it;
            break;
          }

          cerr << "[O] Resolve OK (" << info_.server << "," << info_.hostname << "," << info_.port << "," << endpoint_ << ")" << endl;

          do_handle_socks();
          // do_connect();
        } else {
          cerr << "[O] Resolve failed (" << info_.server << "," << info_.hostname << "," << info_.port << "," << endpoint_ << ")" << endl;
        }
      });
  }

  void do_handle_socks()
  {
    auto self(shared_from_this());

    if (socks_setting_.enable) {
      resolver_.async_resolve(
        string(socks_setting_.hostname),
        string(socks_setting_.port),
        [this, self](boost::system::error_code ec, tcp::resolver::results_type endpoints)
        {
          if (!ec) {
            for (auto it = endpoints.cbegin(); it != endpoints.cend(); it++) {
              socks_endpoint_ = *it;
              break;
            }

            cerr << "[O] SOCKS Resolve OK (" << socks_setting_.hostname << "," << socks_setting_.port << "," << socks_endpoint_ << ")" << endl;
            do_connect_socks();
          } else {
            cerr << "[O] SOCKS Resolve failed (" << socks_setting_.hostname << "," << socks_setting_.port << "," << socks_endpoint_ << ")" << endl;
            do_connect();
          }
        });
    }
  }

  void do_connect_socks()
  {
    auto self(shared_from_this());
  
    socket_.async_connect(
      socks_endpoint_,
      [this, self](boost::system::error_code ec)
      {
        if (!ec) {
          cerr << "[O] Connect OK (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
          
          // Send SOCKS4_REQUEST
          do_send_socks4_request();   
        } else {
          cerr << "[X] Connect failed (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
          do_connect();
        }
      });
  }

  void do_connect()
  {
    auto self(shared_from_this());
  
    socket_.async_connect(
      endpoint_,
      [this, self](boost::system::error_code ec)
      {
        if (!ec) {
          cerr << "[O] Connect OK (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;
          do_read();
        } else {
          cerr << "[X] Connect failed (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;
        }
      });
  }

  void do_send_socks4_request() 
  {
    auto self(shared_from_this());

    SOCKS4_REQUEST req(endpoint_);

    boost::asio::async_write(socket_, boost::asio::buffer((char *)&req.vn, 9),
      [this, self](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (!ec) {
          cerr << "[O] SOCKS4_REQUEST send OK (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
          do_read_socks4_reply();
        } else {
          cerr << "[x] SOCKS4_REQUEST send failed (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
        }
      });
  }

  void do_read_socks4_reply() 
  {
    auto self(shared_from_this());

    socket_.async_read_some(boost::asio::buffer(data_, 8),
      [this, self](boost::system::error_code ec, size_t length)
      {
        if (!ec) {
          cerr << "[O] SOCKS4_REPLY Read OK (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
          
          debug_dump(data_, length);

          if (length != 8) {
            cerr << "[x] SOCKS4_REPLY Read failed: Length error (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
            return;
          }

          SOCKS4_REPLY reply(data_);

          if (reply.vn != 0) {
            cerr << "[x] SOCKS4_REPLY Read failed: VN error (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
            return;
          }

          if (reply.cd != 90) {
            cerr << "[x] SOCKS4_REPLY Read failed: SOCKS4 Server rejected (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
            return;
          }

          // OK, SOCKS4 connection established
          do_read();
        } else {
          cerr << "[x] SOCKS4_REPLY Read failed (" << socks_setting_.hostname << "," << socks_setting_.port << ")" << endl;
        }
      });
  }

  void do_read() 
  {
    auto self(shared_from_this());
    socket_.async_read_some(boost::asio::buffer(data_, max_length),
      [this, self](boost::system::error_code ec, size_t length)
      {
        if (!ec) {
          cerr << "[O] Read OK (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;

          string data = string(data_);
          do_output_shell(data);

          memset(data_, 0, max_length);

          if (data.find("%") != std::string::npos) {
            cerr << "[%] Yes %" << endl;
            do_write();
          } else {
            cerr << "[%] No %" << endl;
            do_read();
          }
        } else {
          cerr << "[x] Read failed (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;
        }
      });
  }

  // Write one line of testcase to np_shell server
  void do_write() 
  {
    cerr << "[D] do_write ..." << endl;
    string data;
    if (!testcase_.size()) {
      return;
    } else {
      data = string(testcase_[0]);
      testcase_.erase(testcase_.begin());
    }

    do_output_command(data);

    cerr << "[D] do_write prepare!!" << endl;

    auto self(shared_from_this());

    cerr << "[D] do_write Fire!!" << endl;
    boost::asio::async_write(socket_, boost::asio::buffer(data.c_str() , data.length()),
      [this, self](boost::system::error_code ec, std::size_t /*length*/)
      {
        if (!ec) {
          cerr << "[O] Write OK (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;
          do_read();
        } else {
          cerr << "[x] Write failed (" << info_.server << "," << info_.hostname << "," << info_.port << ")" << endl;
        }
      });
  }

  /*
  Python code:
    def output_shell(session, content):
      content = html.escape(content)
      content = content.replace('\n', '&NewLine;')
      print(f"<script>document.getElementById('{session}').innerHTML += '{content}';</script>")
      sys.stdout.flush()
  */
  void do_output_shell(string content)
  {
    escape(content);
    string data = "<script>document.getElementById('";
    data += info_.server;
    data += "').innerHTML += '";
    data += content;
    data += "';</script>";
    cout << data;
  }
  
  /*
  Python code:
    def output_command(session, content):
      content = html.escape(content)
      content = content.replace('\n', '&NewLine;')
      print(f"<script>document.getElementById('{session}').innerHTML += '<b>{content}</b>';</script>")
      sys.stdout.flush()
  */
  void do_output_command(string content)
  {
    escape(content);
    string data = "<script>document.getElementById('";
    data += info_.server;
    data += "').innerHTML += '<b>";
    data += content;
    data += "</b>';</script>";
    cout << data;
  }

  void escape(string& str) {
    boost::replace_all(str, "&", "&amp;");
    boost::replace_all(str, "\"", "&quot;");
    boost::replace_all(str, "\'", "&apos;");
    boost::replace_all(str, "<", "&lt;");
    boost::replace_all(str, ">", "&gt;");
    boost::replace_all(str, "\n", "&NewLine;");
    boost::replace_all(str, "\r", "");
  }

  enum { max_length = 1024 };
  char data_[max_length];
  tcp::resolver resolver_;
  tcp::socket socket_;
  connect_info info_;
  socks_info socks_setting_;
  tcp::endpoint endpoint_;
  tcp::endpoint socks_endpoint_;
  vector<string> testcase_;
};

int main()
{
  try
  {
    vector<string> params;
    vector<connect_info> infos(5);
    socks_info socks_setting;
    string index_page = R""""(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <title>NP Project 3 Sample Console</title>
    <link
      rel="stylesheet"
      href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css"
      integrity="sha384-TX8t27EcRE3e/ihU7zmQxVncDAy5uIKz4rEkgIXeMed4M0jlfIDPvg6uqKI2xXr2"
      crossorigin="anonymous"
    />
    <link
      href="https://fonts.googleapis.com/css?family=Source+Code+Pro"
      rel="stylesheet"
    />
    <link
      rel="icon"
      type="image/png"
      href="https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png"
    />
    <style>
      * {
        font-family: 'Source Code Pro', monospace;
        font-size: 1rem !important;
      }
      body {
        background-color: #212529;
      }
      pre {
        color: #cccccc;
      }
      b {
        color: #01b468;
      }
    </style>
  </head>
  <body>
    <table class="table table-dark table-bordered">
    )"""";
/*
      <thead>
        <tr>
          <th scope="col">Server0</th>
          <th scope="col">Server1</th>
          <th scope="col">Server2</th>
          <th scope="col">Server3</th>
          <th scope="col">Server4</th>
        </tr>
      </thead>
      <tbody>
        <tr>
          <td><pre id="s0" class="mb-0"></pre></td>
          <td><pre id="s1" class="mb-0"></pre></td>
          <td><pre id="s2" class="mb-0"></pre></td>
          <td><pre id="s3" class="mb-0"></pre></td>
          <td><pre id="s4" class="mb-0"></pre></td>
        </tr>
      </tbody>
    </table>
  </body>
</html>
*/

    string query = getenv("QUERY_STRING");

    cerr << query << endl;

    cout << "Content-type: text/html\r\n\r\n";
    cout << index_page;

    // Parse "Param1&Param2&Param3"
    boost::split(params, query, boost::is_any_of("&"), boost::token_compress_on);

    for (auto param : params) {
      // Parse "key=value"
      auto split_idx = param.find("=");
              
      if (std::string::npos != split_idx) {
        string key = param.substr(0, split_idx);
        string value = param.substr(split_idx + 1);
        
        int socks = 0;
        int idx = -1;
        
        if (key.length() == 2) {
          if (key[0] == 's') {
            // For SOCKS setting
            socks = 1;
            socks_setting.enable = 1;
          } else {
            int n = key[1] - '0';

            if (0 <= n && n < 5) {
              idx = n;
            }
          }
        }

        if (socks == 1) {
          switch (key[1]) {
            case 'h':
              // Assign SOCKS host
              socks_setting.hostname = value;
              break;
            case 'p':
              // Assign SOCKS port
              socks_setting.port = value;
              break;
          }
        } else if (idx != -1) {
          switch (key[0]) {
            case 'h':
              infos[idx].hostname = value;
              infos[idx].server = "s" + to_string(idx);
              break;
            case 'p':
              infos[idx].port = value;
              break;
            case 'f':
              infos[idx].testcasename = value;
              break;
            default:
              break;
          }
        }
      }
    }
    
    boost::asio::io_context io_context;

    // Make page
    cout << "<thead><tr>";
    for (auto info : infos) {
      if (info.hostname != "") {
        cout << R""""(<th scope="col">)"""";
        cout << info.hostname << ":" << info.port;        
        cout << "</th>";
      }
    }
    cout << "</tr></thead>";
    cout << "<tbody><tr>";
    for (auto info : infos) {
      if (info.hostname != "") {
        cout << R""""(<td><pre id=")"""";
        cout << info.server;       
        cout << R""""(" class="mb-0"></pre></td>)"""";
      }
    }
    cout << "</tr></tbody>";

    for (auto info : infos) {
      if (info.hostname != "") {
        cerr << "[C] (" << info.server << "," << info.hostname << "," << info.port << ")" << endl;
        make_shared<client>(io_context, info, socks_setting)->start();
      }
    }

    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}