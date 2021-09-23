/*
 * Copyright (c) 2021, LIBERTY GLOBAL all rights reserved.
 */

#ifndef I_OMI_PROXY_HPP_
#define I_OMI_PROXY_HPP_

#include <string>
#include <functional>

namespace omi
{

class IOmiProxy
{
public:
    enum class ErrorType { verityFailed };

    typedef std::function<void(const std::string&, ErrorType, const void*)> OmiErrorListener;

    virtual bool init() = 0;

    virtual bool mountCryptedBundle(const std::string& id,
                                    const std::string& rootfs_file_path,
                                    const std::string& config_json_path,
                                    std::string& bundlePath /*out parameter*/) const = 0;

    virtual bool umountCryptedBundle(const std::string& id) const = 0;

    virtual long unsigned registerListener(const OmiErrorListener &listener, const void* cbParams) = 0;

    virtual void unregisterListener(int tag) = 0;

};
}
#endif // #ifndef I_OMI_PROXY_HPP_
