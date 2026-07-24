#include "Ssl.h"

boost::asio::ssl::context sslContext{ boost::asio::ssl::context::tlsv12 };

void initSslContext(std::string certificateFile,std::string privateKeyFile)
{

    sslContext.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::single_dh_use);

    sslContext.use_certificate_chain_file(certificateFile);

    sslContext.use_private_key_file(privateKeyFile, boost::asio::ssl::context::pem);

}

boost::asio::ssl::context& getSslContext()
{

    return sslContext;

}