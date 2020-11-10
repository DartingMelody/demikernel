// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <dmtr/wait.h>

#include <boost/chrono.hpp>
#include <cerrno>
#include <dmtr/annot.h>
#include <dmtr/fail.h>
#include <dmtr/libos.h>
#include <iostream>

//#define DMTR_PROFILE 1

#if DMTR_PROFILE
#   include <dmtr/latency.h>
#endif

#if DMTR_PROFILE
typedef std::unique_ptr<dmtr_latency_t, std::function<void(dmtr_latency_t *)>> latency_ptr_type;
static latency_ptr_type success_poll_latency;
#endif

int dmtr_wait(dmtr_qresult_t *qr_out, dmtr_qtoken_t qt) {
    int ret = EAGAIN;
    while (EAGAIN == ret) {
        ret = dmtr_poll(qr_out, qt);
    }
    DMTR_OK(dmtr_drop(qt));
    return ret;
}

int dmtr_wait_any(dmtr_qresult_t *qr_out, int *ready_offset, dmtr_qtoken_t qts[], int num_qts) {
#if DMTR_PROFILE
    if (NULL == success_poll_latency) {
        dmtr_latency_t *l;
        DMTR_OK(dmtr_new_latency(&l, "dmtr success poll"));
        success_poll_latency = latency_ptr_type(l, [](dmtr_latency_t *latency) {
            dmtr_dump_latency(stderr, latency);
            dmtr_delete_latency(&latency);
        });
    }
#endif
    // start where we last left off
    int i = (ready_offset != NULL && *ready_offset + 1 < num_qts) ? *ready_offset + 1 : 0;
    while (1) {
#if DMTR_PROFILE
        auto t0 = boost::chrono::steady_clock::now();
#endif
        // just ignore zero tokens
        if (qts[i] != 0) {
            int ret = dmtr_poll(qr_out, qts[i]);
            if (ret != EAGAIN) {
                if (ret == 0) {
                    DMTR_OK(dmtr_drop(qts[i]));
#if DMTR_PROFILE
                    auto dt = (boost::chrono::steady_clock::now() - t0);
                    DMTR_OK(dmtr_record_latency(success_poll_latency.get(), dt.count()));
#endif
                    if (ready_offset != NULL)
                        *ready_offset = i;
                    return ret;
                }
            }
        }
        i++;
        if (i == num_qts) i = 0;
    }

    DMTR_UNREACHABLE();
}
