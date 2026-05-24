/**
 * @file pdp_mgr.c
 * @brief PDP 上下文缓存管理
 * @details PDP context cache management
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include <string.h>

#include "esp_check.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "pdp_mgr"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 检查 PDP 上下文 ID 是否有效
 * @details Check whether PDP context ID is valid
 * @param[in] cid PDP 上下文 ID
 * @return
 *         - true: 有效
 *         - false: 无效
 */
static bool cid_valid(uint8_t cid);

/**
 * @brief 获取 PDP 上下文缓存索引
 * @details Get PDP context cache index
 * @param[in] cid PDP 上下文 ID
 * @return PDP 上下文缓存索引
 */
static int cid_index(uint8_t cid);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t pdp_mgr_init(pdp_mgr_t *me, uint8_t primary_cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid_valid(primary_cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid primary cid");

    memset(me, 0, sizeof(*me));
    me->primary_cid = primary_cid;
    for (uint8_t cid = 1; cid <= CORE_MAX_PDP_CONTEXTS; cid++) {
        me->contexts[cid - 1].cid = cid;
    }

    return ESP_OK;
}

esp_err_t pdp_mgr_get(const pdp_mgr_t *me, uint8_t cid,
                      modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG, "invalid cid");

    *pdp = me->contexts[cid_index(cid)];
    return ESP_OK;
}

esp_err_t pdp_mgr_update(pdp_mgr_t *me, const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(pdp->cid), ESP_ERR_INVALID_ARG, TAG, "invalid cid");

    me->contexts[cid_index(pdp->cid)] = *pdp;
    return ESP_OK;
}

esp_err_t pdp_mgr_set_active(pdp_mgr_t *me, uint8_t cid, bool active)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG, "invalid cid");

    int index = cid_index(cid);

    me->contexts[index].cid = cid;
    me->contexts[index].active = active;
    return ESP_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool cid_valid(uint8_t cid)
{
    return cid >= 1 && cid <= CORE_MAX_PDP_CONTEXTS;
}

static int cid_index(uint8_t cid)
{
    return (int)cid - 1;
}
