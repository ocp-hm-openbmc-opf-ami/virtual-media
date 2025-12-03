#pragma once

// #include "logging.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <string>

// Wrapper for boost::async_pipe ensuring proper pipe cleanup
class Credentials
{
  public:
    explicit Credentials(boost::asio::io_context& io, std::string url = "",
                         bool rw_status = false,
                         std::string additionalInfo = "") :
        impl(io), read(io), url(std::move(url)), rw_status(rw_status),
        additionalInfo(std::move(additionalInfo))
    {
        boost::system::error_code ec;
        boost::asio::connect_pipe(read, impl, ec);
        if (ec)
        {
            // BMCWEB_LOG_CRITICAL("Failed to connect pipe {}", ec.what());
        }
    }

    Credentials(const Credentials&) = delete;
    Credentials(Credentials&&) = delete;
    Credentials& operator=(const Credentials&) = delete;
    Credentials& operator=(Credentials&&) = delete;

    ~Credentials()
    {
        explicit_bzero(user.data(), user.capacity());
        explicit_bzero(pass.data(), pass.capacity());
    }

    int releaseFd()
    {
        return read.release();
    }

    template <typename WriteHandler>
    void asyncWrite(std::string&& username, std::string&& password,
                    WriteHandler&& handler)
    {
        user = std::move(username);
        pass = std::move(password);

        // Add +1 to ensure that the null terminator is included.
        std::array<boost::asio::const_buffer, 2> buffer{
            {{user.data(), user.size() + 1}, {pass.data(), pass.size() + 1}}};
        boost::asio::async_write(impl, buffer,
                                 std::forward<WriteHandler>(handler));
    }

    std::string getUrl() const
    {
        return url;
    }
    bool getRwStatus() const
    {
        return rw_status;
    }
    std::string getAdditionalInfo() const
    {
        return additionalInfo;
    }

    boost::asio::writable_pipe impl;
    boost::asio::readable_pipe read;

  private:
    std::string url;
    bool rw_status;
    std::string user;
    std::string pass;
    std::string additionalInfo;
};
