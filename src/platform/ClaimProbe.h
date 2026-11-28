// SPDX-License-Identifier: GPL-3.0-or-later
// Dev-only staging-claim probe (M3 Increment 7), compiled only into DEBUGTOOLS builds (`./dev run --claimprobe` builds that variant by
// itself). A release build has neither the code nor the strings. Devstart key `claimprobe=1`, environment OPENSHOP_CLAIMPROBE.
//
// WHAT IT SHOWS, on the SD card (sd:/apps/OpenShop/staging/), through the REAL install::Jobs queue and install worker: a job that
// claims a staging dir (RunCtx::claimStaging) is not touched by the Cleanup routine, an abandoned dir is, and the claim ends with
// the job. It never blocks the main thread: probeClaim() only queues three jobs and returns, the UI keeps drawing.
//   job A (a runner on the install worker)  refuses if a journal exists on sd: or staging already holds dirs the probe did not create
//                  (logged, skipped). Otherwise claims staging/<claimed>, creates it and staging/<abandoned> (each with a few small files and
//                  a marker file; ids from freshJobId), then, WHILE STILL HOLDING the claim, runs the Cleanup routine the Cleanup job uses
//                  (RunCtx::cleanup: cleanStaging with the real active-claims guard) and logs
//                      claimprobe: claimed dir kept=1 abandoned dir removed=1
//   job B          a normally queued Cleanup job for sd: (the claim was released when A returned; B removes the claimed dir)
//   job C          verifies the claimed dir is gone, logs "claimprobe: RESULT PASS" or "claimprobe: RESULT FAIL <why>", removes anything the
//                  probe created that is still there and logs "claimprobe: cleaned" (or "NOT CLEAN").
// Every log line starts with "claimprobe:".
#pragma once
#ifdef OPENSHOP_DEBUGTOOLS
namespace install { class Jobs; }

namespace platform {
// Queues the three jobs; false (and a "claimprobe: REFUSED" line) when there is no writable SD root or the job system is not running.
bool claimProbe(install::Jobs& jobs);
} // namespace platform
#endif
