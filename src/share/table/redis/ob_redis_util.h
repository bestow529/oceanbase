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

#ifndef OCEANBASE_SHARE_TABLE_OB_REDIS_UTIL_H_
#define OCEANBASE_SHARE_TABLE_OB_REDIS_UTIL_H_

#include "lib/utility/ob_macro_utils.h"
#include "share/table/ob_table.h"
#include "lib/oblog/ob_log_module.h"
#include "observer/table/ob_table_rpc_processor_util.h"
#include "share/table/ob_table_rpc_struct.h"
#include "share/table/redis/ob_redis_common.h"
#include "lib/utility/ob_fast_convert.h"
#include "observer/table/ob_table_context.h"

namespace oceanbase
{
namespace table
{
class ObRedisRequest;
// Format conversion in redis protocol and obtable
class ObRedisHelper
{
public:
  static const uint64_t MAX_BULK_LEN = 1 * 1024 * 1024; // 1M, not compatible with redis (512M)

  // static const int64_t MAX_DBL_STR_LEN = 5 * 1024; // compatible with redis
  /**
   * Get the decoded message from the entity
   * @param entity Entity object
   * @param decode_msgs The decoded array of messages
   * @return retcode
   */
  static int get_decoded_msg_from_entity(ObIAllocator &allocator,
                                         const ObITableEntity &entity,
                                         ObString &cmd_name,
                                         ObArray<ObString> &args);

  static observer::ObTableProccessType redis_cmd_to_proccess_type(const RedisCommandType &redis_cmd_type);

  static int get_lock_key(ObIAllocator &allocator, const ObRedisRequest &redis_req, ObString &lock_key);
  static int get_table_name_by_model(ObRedisModel model, ObString &table_name);
  static int model_to_string(ObRedisModel model, ObString &str);
  static int check_redis_ttl_schema(const ObTableSchema &table_schema, const table::ObRedisModel redis_model);
  static int string_to_double(const ObString &str, double &d);
  static int double_to_string(ObIAllocator &allocator, double d, ObString &str);
  static int string_to_long_double(const ObString &str, long double &ld);
  static int long_double_to_string(ObIAllocator &allocator, long double ld, ObString &str);
  template<class T>
  static int get_int_from_str(const common::ObString &int_str, T &int_res);
  static int gen_meta_scan_range(ObIAllocator &allocator, const ObTableCtx &tb_ctx, ObIArray<ObNewRange> &scan_ranges);
  static int check_string_length(int64_t old_len, int64_t append_len);
  static int str_to_lower(ObIAllocator &allocator, const ObString &src, ObString &dst);

private:
  static int init_str2d(ObIAllocator &allocator, double d, char *&buf, size_t &length);
};

// Encapsulate the input and output parameters of redis
class ObRedisRequest
{
public:
  explicit ObRedisRequest(common::ObIAllocator &allocator, const ObTableOperationRequest &request)
      : allocator_(allocator),
        request_(request),
        cmd_name_(),
        args_(OB_MALLOC_NORMAL_BLOCK_SIZE, ModulePageAllocator(allocator, "RedisArgs")),
        db_(-1)
  {}
  ~ObRedisRequest()
  {}
  TO_STRING_KV(K_(request), K_(cmd_name), K_(args), K_(db));
  // Parse request_ as cmd_name_, args_
  int decode();

  OB_INLINE const ObString& get_cmd_name() const
  {
    return cmd_name_;
  }

  OB_INLINE const char* get_lower_cmd_name_ctr() const
  {
    ObString lower_cmd_name;
    ObRedisHelper::str_to_lower(allocator_, cmd_name_, lower_cmd_name);
    return lower_cmd_name.ptr();
  }

  OB_INLINE const ObArray<ObString> &get_args() const
  {
    return args_;
  }

  OB_INLINE const ObTableOperationRequest &get_request() const
  {
    return request_;
  }

  OB_INLINE const ObITableEntity &get_entity() const
  {
    return request_.table_operation_.entity();
  }

  OB_INLINE int64_t get_db() const
  {
    return db_;
  }

  OB_INLINE const ObString &get_table_name() const
  {
    return request_.table_name_;
  }

private:
  common::ObIAllocator &allocator_;
  const ObTableOperationRequest &request_;
  ObString cmd_name_;
  ObArray<ObString> args_;
  // all redis command include db info
  int64_t db_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObRedisRequest);
};

class ObRedisResponse
{
public:
  ObRedisResponse(common::ObIAllocator &allocator, ObTableOperationResult &result)
      : allocator_(allocator),
        result_(result)
  {}
  ~ObRedisResponse()
  {}
  TO_STRING_KV(K_(result));
  void reset() {
    result_.reset();
    allocator_.reset();
  }
  OB_INLINE ObTableOperationResult &get_result()
  {
    return result_;
  }
  // set res to redis fmt
  int set_res_int(int64_t res);
  int set_res_simple_string(const ObString &res);
  int set_res_bulk_string(const ObString &res);
  int set_res_array(const ObIArray<ObString> &res);
  int set_res_null();
  int set_res_error(const ObString &err_msg);
  // table_api failed, return error code
  void return_table_error(int ret);
  // Some common hard-coded redis protocol return values, directly returned
  int set_fmt_res(const ObString &fmt_res);
  OB_INLINE common::ObIAllocator& get_allocator() { return allocator_; }

private:
  common::ObIAllocator &allocator_;
  ObTableOperationResult &result_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObRedisResponse);
};

template<class T>
int ObRedisHelper::get_int_from_str(const common::ObString &int_str, T &res_int)
{
  int ret = OB_SUCCESS;
  bool is_valid = true;
  if (int_str.length() == 0) {
    is_valid = false;
  } else if (int_str.length() > 1) {
    // not allow '00001' or '-0'/'+123' in redis
    if (int_str[0] == '0' || int_str[0] == '+') {
      is_valid = false;
    } else if (int_str[0] == '-' && int_str[1] == '0') {
      is_valid = false;
    }
  }
  if (is_valid) {
    res_int = common::ObFastAtoi<T>::atoi(int_str.ptr(), int_str.ptr() + int_str.length(), is_valid);
  }

  if (!is_valid) {
    ret = OB_CONVERT_ERROR;
    SERVER_LOG(WARN, "value is not an integer or out of range", K(ret), K(int_str));
  }

  return ret;
}
}  // end namespace table
}  // end namespace oceanbase

#endif /* OCEANBASE_SHARE_TABLE_OB_REDIS_UTIL_H_ */