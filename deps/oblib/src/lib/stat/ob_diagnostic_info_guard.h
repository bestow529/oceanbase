/**
 * Copyright (c) 2024 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OB_DIAGNOSTIC_INFO_GUARD_H_
#define OB_DIAGNOSTIC_INFO_GUARD_H_

#include "lib/utility/ob_macro_utils.h"
#include "lib/wait_event/ob_wait_event.h"
#include "lib/statistic_event/ob_stat_event.h"
#include "lib/ash/ob_execution_phase.h"
#include "lib/stat/ob_diagnostic_info.h"
#include "lib/stat/ob_diagnostic_info_summary.h"

namespace oceanbase
{

#define GET_DIAGNOSTIC_INFO                              \
  if (oceanbase::common::ObLocalDiagnosticInfo::get() != \
      &oceanbase::common::ObDiagnosticInfo::dummy_di_)    \
  oceanbase::common::ObLocalDiagnosticInfo::get()

namespace observer
{
class ObInnerSqlWaitGuard;
}
namespace sql
{
class ObEndTransAsyncCallback;
}

namespace common
{

class ObDiagnosticInfo;
class ObDiagnosticInfoSlot;
class ObDiagnosticInfoContainer;
class ObDiagnosticInfoSwitchGuard;
class ObLatchStat;

class ObBackGroundSessionGuard
{
public:
  ObBackGroundSessionGuard(int64_t tenant_id, int64_t group_id);
  ~ObBackGroundSessionGuard();

private:
  ObDiagnosticInfo *di_;
  bool prev_value_;
  ObDiagnosticInfoContainer *dic_;
};

class ObLocalDiagnosticInfo
{
public:
  friend class ObTenantDiagnosticInfoSummaryGuard;
  friend class ObDiagnosticInfoSwitchGuard;
  friend class ObBackGroundSessionGuard;
  friend class observer::ObInnerSqlWaitGuard;
  friend class sql::ObEndTransAsyncCallback;
  DISABLE_COPY_ASSIGN(ObLocalDiagnosticInfo);
  static inline ObDiagnosticInfo *get()
  {
    return get_instance().get_diagnostic_ptr();
  }
  static int aggregate_diagnostic_info_summary(ObDiagnosticInfo *di);
  static int revert_diagnostic_info(ObDiagnosticInfo *di);
  static int return_diagnostic_info(ObDiagnosticInfo *di);
  static inline void add_stat(ObStatEventIds::ObStatEventIdEnum stat_no, int64_t value)
      __attribute__((always_inline))
  {
    ObLocalDiagnosticInfo &instance = get_instance();
    ObDiagnosticInfoSlot *slot = instance.slot_;
    if (OB_UNLIKELY(nullptr != slot)) {
      slot->atomic_add_stat(stat_no, value);
    } else {
      instance.get_diagnostic_ptr()->add_stat(stat_no, value);
    }
  }
  static ObLatchStat *get_latch_stat(int64_t latch_id);
  static void set_thread_name(const char *name);
  static void set_thread_name(uint64_t tenant_id, const char *name);
  static void set_service_action(const char *program, const char *module, const char *action);
  static inline int inc_ref(ObDiagnosticInfo *di) __attribute__((always_inline))
  {
    int ret = OB_SUCCESS;
    if (OB_NOT_NULL(di)) {
      ATOMIC_INC(&di->ref_cnt_);
    }
    return ret;
  }
  static int dec_ref(ObDiagnosticInfo *di);
private:
  ObLocalDiagnosticInfo();
  ~ObLocalDiagnosticInfo() = default;
  static inline ObLocalDiagnosticInfo &get_instance()
  {
    static thread_local ObLocalDiagnosticInfo di;
    return di;
  }
  inline ObDiagnosticInfo *&get_diagnostic_ptr()
  {
    return di_ptr_;
  }
  static void setup_di_slot(ObDiagnosticInfoSlot *di_slot)
  {
    get_instance().slot_ = di_slot;
  }
  static void reset_di_slot()
  {
    get_instance().slot_ = nullptr;
  }
  static inline void reset_diagnostic_info()
  {
    get_instance().get_diagnostic_ptr() = &ObDiagnosticInfo::dummy_di_;
  }
  static inline void setup_diagnostic_info(ObDiagnosticInfo *di)
  {
    if (OB_NOT_NULL(di)) {
      get_instance().get_diagnostic_ptr() = di;
      di->get_ash_stat().tid_ = GETTID();
    } else {
      COMMON_LOG_RET(ERROR, OB_ERR_UNEXPECTED, "set nullptr to local diagnostic info", K(lbt()));
    }
  }
  ObDiagnosticInfo *di_ptr_ CACHE_ALIGNED;
  ObDiagnosticInfoSlot *slot_ CACHE_ALIGNED;
};

class ObTenantDiagnosticInfoSummaryGuard
{
public:
  explicit ObTenantDiagnosticInfoSummaryGuard(int64_t tenant_id, int64_t group_id = 0,
      bool using_global = false /*ATTENTION: do not set this unless you know what you are doing*/);
  explicit ObTenantDiagnosticInfoSummaryGuard(ObDiagnosticInfoSlot *slot);
  ~ObTenantDiagnosticInfoSummaryGuard();
  DISABLE_COPY_ASSIGN(ObTenantDiagnosticInfoSummaryGuard);

private:
  bool need_restore_slot_;
  ObDiagnosticInfoSlot *prev_slot_;
};

class ObDiagnosticInfoSwitchGuard
{
public:
  explicit ObDiagnosticInfoSwitchGuard(ObDiagnosticInfo *di);
  ~ObDiagnosticInfoSwitchGuard();
  DISABLE_COPY_ASSIGN(ObDiagnosticInfoSwitchGuard);

private:
  ObDiagnosticInfo *prev_di_;
  ObDiagnosticInfo *cur_di_;
  bool di_switch_success_;
#ifdef ENABLE_DEBUG_LOG
  bool leak_check_;
#endif
  bool prev_value_;
};

class ObRetryWaitEventInfoGuard {
public:
  ObRetryWaitEventInfoGuard(sql::ObSQLSessionInfo &session);
  ~ObRetryWaitEventInfoGuard();

private:
  bool is_switch_;
  sql::ObQueryRetryASHDiagInfo *parent_ptr_;
};

} /* namespace common */
} /* namespace oceanbase */

#define EVENT_ADD(stat_no, value)                                                         \
  do {                                                                                    \
    if (oceanbase::lib::is_diagnose_info_enabled()) {                                     \
      ObLocalDiagnosticInfo::add_stat(oceanbase::common::ObStatEventIds::stat_no, value); \
    }                                                                                     \
  } while (0)

#define EVENT_TENANT_ADD(stat_no, value, tenant_id)                \
  oceanbase::common::ObTenantStatEstGuard tenant_guard(tenant_id); \
  EVENT_ADD(stat_no, value);

#define EVENT_INC(stat_no) EVENT_ADD(stat_no, 1)

#define EVENT_TENANT_INC(stat_no, tenant_id) EVENT_TENANT_ADD(stat_no, 1, tenant_id)

#define EVENT_DEC(stat_no) EVENT_ADD(stat_no, -1)

#define WAIT_BEGIN(event_no, timeout_ms, p1, p2, p3, is_atomic) \
  do {                                                          \
    if (oceanbase::lib::is_diagnose_info_enabled()) {           \
      need_record_ = true;                                      \
      ObDiagnosticInfo *di = ObLocalDiagnosticInfo::get();      \
      OB_ASSERT(di != &ObDiagnosticInfo::dummy_di_);            \
      di->begin_wait_event(event_no, timeout_ms, p1, p2, p3);   \
    } else {                                                    \
      need_record_ = false;                                     \
    }                                                           \
  } while (0)

#define WAIT_END(event_no)                                                                        \
  do {                                                                                            \
    if (need_record_) {                                                                           \
      ObDiagnosticInfo *di = ObLocalDiagnosticInfo::get();                                        \
      di->end_wait_event(event_no, OB_WAIT_EVENTS[event_no].wait_class_ == ObWaitClassIds::IDLE); \
    }                                                                                             \
  } while (0)

#endif /* OB_DIAGNOSTIC_INFO_GUARD_H_ */