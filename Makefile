CXX=g++

# Add -DDEBUG for debug log
CXXFLAGS=-std=c++11 -Wall -pedantic -pthread -lboost_system -lboost_filesystem -DBOOST_NO_CXX11_SCOPED_ENUMS

CXX_INCLUDE_DIRS=/usr/local/include
CXX_INCLUDE_PARAMS=$(addprefix -I , $(CXX_INCLUDE_DIRS))
CXX_LIB_DIRS=/usr/local/lib
CXX_LIB_PARAMS=$(addprefix -L , $(CXX_LIB_DIRS))

SOCKS_SERVER = socks_server
SOCKS_SERVER_SRC = ./socks_server_dir/src

HW4_CGI = hw4.cgi
HW4_CGI_SRC = ./cgi_dir/src

all: $(SOCKS_SERVER) $(HW4_CGI)
	
$(SOCKS_SERVER):
	@echo "Compiling" $@ "..."
	$(CXX) $(SOCKS_SERVER_SRC)/socks_server.cpp -o $@ $(CXX_INCLUDE_PARAMS) $(CXX_LIB_PARAMS) $(CXXFLAGS)

$(HW4_CGI):
	@echo "Compiling" $@ "..."
	$(CXX) $(HW4_CGI_SRC)/hw4.cpp -o $@ $(CXX_INCLUDE_PARAMS) $(CXX_LIB_PARAMS) $(CXXFLAGS)

clean:
	rm -f $(SOCKS_SERVER)
	rm -f $(HW4_CGI)