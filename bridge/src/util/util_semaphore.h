/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#ifndef UTIL_SEMAPHORE_H_
#define UTIL_SEMAPHORE_H_

#include "util_common.h"
#include "log/log.h"
#include "util_guid.h"

#include <windows.h>
#include <sstream>

#define FIVE_SECONDS 5'000
#define HALF_SECOND 500
#define QUARTER_SECOND 250

extern bridge_util::Guid gUniqueIdentifier;

namespace bridge_util {

  // Named semaphore for cross-process synchronization
  class NamedSemaphore {
    std::string baseName;
    const size_t count;
    size_t avail;
    HANDLE ghSemaphore = nullptr;

  public:
    NamedSemaphore(const std::string& name, const size_t& init, const size_t& max)
      : baseName(name)
      , count(max)
      , avail(init) {

      const auto uniqueName = gUniqueIdentifier.toString(name);

      // CreateSemaphore will either create OR open an existing named semaphore.
      // Clear the thread's last-error value so ERROR_ALREADY_EXISTS is unambiguous.
      SetLastError(ERROR_SUCCESS);
      ghSemaphore = CreateSemaphore(
        nullptr,                    // default access
        static_cast<LONG>(avail),   // initial count
        static_cast<LONG>(count),   // maximum count
        uniqueName.c_str());        // named semaphore

      if (ghSemaphore == nullptr) {
        const DWORD err = GetLastError();
        std::stringstream ss;
        ss << "CreateSemaphore failed with error code " << err << " (0x" << std::hex << err << ")\\n";
        Logger::err(ss.str());
      } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::debug(format_string("CreateSemaphore returned existing semaphore by the same name %s.", name.c_str()));
      }
    }

    ~NamedSemaphore() {
      if (ghSemaphore != nullptr) {
        CloseHandle(ghSemaphore);
      }
    }

    bool isValid() const {
      return ghSemaphore != nullptr;
    }

    NamedSemaphore(const NamedSemaphore& s) = delete;

    Result wait();
    Result wait(const DWORD timeoutMS);

    void release(LONG batchSize = 1);

    size_t available() const {
      return avail;
    }
  };

}

#endif // UTIL_SEMAPHORE_H_
