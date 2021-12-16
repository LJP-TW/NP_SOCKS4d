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
#include <boost/asio.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string.hpp>

using tcp = boost::asio::ip::tcp;
using namespace std;

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
  client(boost::asio::io_context& io_context, connect_info info)
    : resolver_(boost::asio::make_strand(io_context)),
      socket_(io_context)
  {
    info_ = info;
    
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
  }

  void start()
  {
    do_resolve();
  }

private:
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

          do_connect();
        } else {
          cerr << "[O] Resolve failed (" << info_.server << "," << info_.hostname << "," << info_.port << "," << endpoint_ << ")" << endl;
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
  tcp::endpoint endpoint_;
  vector<string> testcase_;
};

int main()
{
  try
  {
    vector<string> params;
    vector<connect_info> infos(5);
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
        
        int idx = -1;
        
        if (key.length() == 2) {
          int n = key[1] - '0';
          if (0 <= n && n < 5) {
            idx = n;
          }
        }

        if (idx != -1) {
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
        make_shared<client>(io_context, info)->start();
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