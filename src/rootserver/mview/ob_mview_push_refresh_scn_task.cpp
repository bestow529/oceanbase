/**
 * Copyright (c) 2023 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX RS

#include "observer/dbms_scheduler/ob_dbms_sched_job_utils.h"
#include "observer/ob_server_struct.h"
#include "observer/omt/ob_multi_tenant.h"
#include "rootserver/mview/ob_mview_push_refresh_scn_task.h"
#include "share/ob_errno.h"
#include "share/schema/ob_mview_info.h"
#include "share/schema/ob_mview_refresh_stats_params.h"
#include "share/ob_global_stat_proxy.h"
#include "share/ob_freeze_info_proxy.h"
#include "share/backup/ob_backup_data_table_operator.h"
#include "storage/mview/ob_mview_refresh_stats_purge.h"

namespace oceanbase {
namespace rootserver {

#define QUERY_MAJOR_MV_MERGE_SCN_SQL "select mview_id,t2.data_table_id,last_refresh_scn,t3.tablet_id, \
  t4.svr_ip,t4.svr_port,t4.ls_id,t5.learner_list, t4.end_log_scn from %s t1 \
  left join %s t2 on t1.mview_id = t2.table_id \
  left join %s t3 on t2.data_table_id = t3.table_id \
  left join %s t4 on t3.tablet_id = t4.tablet_id and t4.table_type = 10 \
  left join %s t5 on t4.svr_ip = t5.svr_ip and t4.svr_port = t5.svr_port and t4.ls_id = t5.ls_id \
  where t1.refresh_mode = %ld and t1.last_refresh_scn > 0 order by 1,2,3,4,5,6,7,9"


ObMViewPushRefreshScnTask::ObMViewPushRefreshScnTask()
  : is_inited_(false),
    in_sched_(false),
    is_stop_(true),
    tenant_id_(OB_INVALID_TENANT_ID)
{
}

ObMViewPushRefreshScnTask::~ObMViewPushRefreshScnTask() {}

int ObMViewPushRefreshScnTask::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObMViewPushRefreshScnTask init twice", KR(ret), KP(this));
  } else {
    tenant_id_ = MTL_ID();
    is_inited_ = true;
  }
  return ret;
}

int ObMViewPushRefreshScnTask::start()
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObMViewPushRefreshScnTask not init", KR(ret), KP(this));
  } else {
    is_stop_ = false;
    if (!in_sched_ &&
               OB_FAIL(schedule_task(MVIEW_PUSH_REFRESH_SCN_INTERVAL, true /*repeat*/))) {
      LOG_WARN("fail to schedule ObMViewPushRefreshScnTask", KR(ret));
    } else {
      in_sched_ = true;
    }
  }
  return ret;
}

void ObMViewPushRefreshScnTask::stop()
{
  is_stop_ = true;
  in_sched_ = false;
  cancel_task();
}

void ObMViewPushRefreshScnTask::destroy()
{
  is_inited_ = false;
  is_stop_ = true;
  in_sched_ = false;
  cancel_task();
  wait_task();
  tenant_id_ = OB_INVALID_TENANT_ID;
}

void ObMViewPushRefreshScnTask::wait() { wait_task(); }

void ObMViewPushRefreshScnTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  uint64_t data_version = 0;
  common::ObISQLClient *sql_proxy = GCTX.sql_proxy_;
  bool need_schedule = false;
  ObMySQLTransaction trans;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObMViewPushRefreshScnTask not init", KR(ret), KP(this));
  } else if (OB_UNLIKELY(is_stop_)) {
    // do nothing
  } else if (OB_FAIL(need_schedule_major_refresh_mv_task(tenant_id_, need_schedule))) {
    LOG_WARN("fail to check need schedule major refresh mv task", KR(ret), K(tenant_id_));
  } else if (!need_schedule) {
  } else if (FALSE_IT(void(check_major_mv_refresh_scn_safety(tenant_id_)))) {
  } else if (OB_UNLIKELY(OB_ISNULL(sql_proxy))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", KR(ret));
  } else if (OB_FAIL(trans.start(sql_proxy, tenant_id_))) {
    LOG_WARN("fail to start trans", KR(ret), K(tenant_id_));
  } else {
    share::ObGlobalStatProxy stat_proxy(trans, tenant_id_);
    share::SCN major_refresh_mv_merge_scn;
    ObArray<share::ObBackupJobAttr> backup_jobs;
    uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
    if (OB_FAIL(stat_proxy.get_major_refresh_mv_merge_scn(true /*select for update*/,
                                                          major_refresh_mv_merge_scn))) {
      LOG_WARN("fail to get major_refresh_mv_merge_scn", KR(ret), K(tenant_id_));
    } else if (OB_UNLIKELY(!major_refresh_mv_merge_scn.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("major_refresh_mv_merge_scn is invalid", KR(ret), K(tenant_id_),
                K(major_refresh_mv_merge_scn));
    } else if (OB_FAIL(share::ObBackupJobOperator::get_jobs(
                   *sql_proxy, meta_tenant_id, false /*select for update*/, backup_jobs))) {
      LOG_WARN("failed to get backup jobs", K(ret), K(tenant_id_));
    } else if (!backup_jobs.empty()) {
      LOG_INFO("[MAJ_REF_MV] backup jobs exist, skip push major refresh mv scn", KR(ret),
               K(tenant_id_));
    } else if (OB_FAIL(ObMViewInfo::update_major_refresh_mview_scn(trans, tenant_id_,
                                                                   major_refresh_mv_merge_scn))) {
      LOG_WARN("fail to update major_refresh_mview_scn", KR(ret), K(tenant_id_),
                K(major_refresh_mv_merge_scn));
    } else {
      LOG_INFO("[MAJ_REF_MV] successfully push major refresh mview refresh scn", K(tenant_id_),
                K(major_refresh_mv_merge_scn));
    }
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("failed to commit trans", KR(ret), KR(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
}

int ObMViewPushRefreshScnTask::check_major_mv_refresh_scn_safety(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  common::ObISQLClient *sql_proxy = GCTX.sql_proxy_;
  ObArray<ObMajorMVMergeInfo> merge_info_array;
  if (OB_FAIL(trans.start(sql_proxy, tenant_id))) {
    LOG_WARN("fail to start trans", KR(ret), K(tenant_id));
  } else {
    share::ObGlobalStatProxy stat_proxy(trans, tenant_id);
    share::SCN major_refresh_mv_merge_scn;
    const bool select_for_update = true;
    if (OB_FAIL(stat_proxy.get_major_refresh_mv_merge_scn(select_for_update,
                                                          major_refresh_mv_merge_scn))) {
      LOG_WARN("fail to get major_refresh_mv_merge_scn", KR(ret), K(tenant_id));
    } else if (OB_FAIL(get_major_mv_merge_info_(tenant_id, trans, merge_info_array))) {
      LOG_WARN("fail to get major_mv merge_info", KR(ret));
    }
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("failed to commit trans", KR(ret), KR(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  if (OB_SUCC(ret)) {
    LOG_INFO("major_mv_safety>>>>");
    bool is_safety = true;
    for (int64_t idx = 0; OB_SUCC(ret) && idx < merge_info_array.count(); idx++) {
      ObMajorMVMergeInfo &merge_info = merge_info_array.at(idx);
      LOG_INFO("major_mv_safety>>>> merge_info", K(merge_info));
      if (!merge_info.is_valid()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("merge_info is invalid", K(merge_info));
      } else if (idx == merge_info_array.count() - 1 || !merge_info.is_equal_node(merge_info_array.at(idx+1))) {
        bool find_dest_merge_scn = false;
        for (int64_t i = 0; i < merge_info_array.count(); i++) {
          ObMajorMVMergeInfo &tmp_merge_info = merge_info_array.at(i);
          if (merge_info.is_equal_node(tmp_merge_info)) {
             if (merge_info.last_refresh_scn_ == tmp_merge_info.end_log_scn_) {
               find_dest_merge_scn = true;
               break;
             }
          }
        }
        if (find_dest_merge_scn) {
        } else if (merge_info.has_learner_) {
          LOG_WARN("major_mv_safety>>>>", K(merge_info));
        } else {
          LOG_ERROR("major_mv_safety>>>>", K(merge_info));
          is_safety = false;
        }
      }
    }
    LOG_INFO("major_mv_safety>>>>", K(is_safety));
  }
  return ret;
}

int ObMViewPushRefreshScnTask::get_major_mv_merge_info_(const uint64_t tenant_id,
                                                        ObISQLClient &sql_client,
                                                        ObIArray<ObMajorMVMergeInfo> &merge_info_array)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(QUERY_MAJOR_MV_MERGE_SCN_SQL,
          share::OB_ALL_MVIEW_TNAME,
          share::OB_ALL_TABLE_TNAME,
          share::OB_ALL_TABLET_TO_LS_TNAME,
          share::OB_ALL_VIRTUAL_TABLE_MGR_TNAME,
          share::OB_ALL_VIRTUAL_LOG_STAT_TNAME,
          (int64_t)ObMVRefreshMode::MAJOR_COMPACTION))) {
    LOG_WARN("assign sql failed", KR(ret));
  } else {
    common::sqlclient::ObMySQLResult *result = nullptr;
    SMART_VAR(ObMySQLProxy::MySQLResult, res)
    {
      common::sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_client.read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", KR(ret));
      } {
        bool is_result_next_err = true;
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          ObMajorMVMergeInfo merge_info;
          char svr_ip[OB_IP_STR_BUFF] = "";
          int64_t svr_port = 0;
          int64_t tmp_real_str_len = 0;
          ObString learner_list;
          EXTRACT_INT_FIELD_MYSQL(*result, "mview_id", merge_info.mview_id_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "data_table_id", merge_info.data_table_id_, int64_t);
          EXTRACT_UINT_FIELD_MYSQL(*result, "last_refresh_scn", merge_info.last_refresh_scn_, uint64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "tablet_id", merge_info.tablet_id_, int64_t);
          EXTRACT_STRBUF_FIELD_MYSQL(*result, "svr_ip", svr_ip, OB_IP_STR_BUFF, tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL(*result, "svr_port", svr_port, int64_t);
          (void)merge_info.svr_addr_.set_ip_addr(svr_ip, static_cast<int32_t>(svr_port));
          EXTRACT_INT_FIELD_MYSQL(*result, "ls_id", merge_info.ls_id_, int64_t);
          EXTRACT_UINT_FIELD_MYSQL(*result, "end_log_scn", merge_info.end_log_scn_, uint64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "learner_list", learner_list);
          if (learner_list.length() > 0) {
            merge_info.has_learner_ = true;
          } else {
            merge_info.has_learner_ = false;
          }

          if (OB_FAIL(ret)) {
            LOG_WARN("fail to extract field from result", KR(ret));
          } else if (OB_FAIL(merge_info_array.push_back(merge_info))) {
            LOG_WARN("fail to push merge_info to array", KR(ret));
          }
          if (OB_FAIL(ret)) {
            is_result_next_err = false;
          }
        }
        if (OB_ITER_END == ret && is_result_next_err) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

} // namespace rootserver
} // namespace oceanbase