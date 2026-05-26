# Air780EP CME ERROR 错误码参考

本文件完整复刻自合宙文档中心的 [CME ERROR — at@air780ep](https://docs.openluat.com/air780ep/at/app/Command_List/Configuration/CME_ERROR/)，适用于 Air780EP (Cat.1) 模块的 `+CME ERROR: <err>` 响应码查询。

## 格式说明

AT 命令出错时，模块返回 `+CME ERROR: <err>`，其中 `<err>` 为数字型错误码。以下表格包含三列：

- **错误码**：`<err>` 数字取值
- **冗长码**：`<err>` 文本取值
- **解释**：中文说明（部分条目原文空缺）

> Air780EP 属 Cat.1 模块。表中标注"Cat.1"的条目为 Cat.1 特有；标注"Cat.4"的仅为 Cat.4 模块适用，Air780EP 不涉及。

---

## 通用错误 (0–100)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 0 | phone failure | 手机故障 |
| 1 | no connection to phone | 未连接到手机 |
| 2 | phone-adaptor link reserved | 预留手机适配器链路 |
| 3 | operation not allowed | 不允许操作 |
| 4 | operation not supported | 不支持操作 |
| 5 | PH-SIM PIN required | 需要PH-SIM卡的PIN |
| 6 | PH-FSIM PIN required | 需要PH-FSIM的PIN |
| 7 | PH-FSIM PUK required | 需要PH-FSIM的PUK |
| 10 | SIM not inserted | 没有插入SIM卡 |
| 11 | SIM PIN required | 需要SIM卡的PIN |
| 12 | SIM PUK required | 需要SIM卡的PUK |
| 13 | SIM failure | SIM卡故障 |
| 14 | SIM busy | SIM卡遇忙 |
| 15 | SIM wrong | SIM错误 |
| 16 | incorrect password | 密码无效 |
| 17 | SIM PIN2 required | 需要SIM卡的PIN2 |
| 18 | SIM PUK2 required | 需要SIM卡的PUK2 |
| 20 | memory full | 存储已满 |
| 21 | invalid index | 索引无效 |
| 22 | not found | 未发现 |
| 23 | memory failure | 存储故障 |
| 24 | text string too long | 文本字符串过长 |
| 25 | invalid characters in text string | 文本字符串中的字符无效 |
| 26 | dial string too long | 拨号字符串过长 |
| 27 | invalid characters in dial string | 拨号字符串中的字符无效 |
| 30 | no network service | 无网络业务 |
| 31 | network timeout | 网络超时 |
| 32 | network not allowed - emergency calls only | 网络不允许－只适用于紧急呼叫 |
| 40 | network personalization PIN required | 需要网络个性化PIN |
| 41 | network personalization PUK required | 需要网络个性化PUK |
| 42 | network subset personalization PIN required | 需要网络子集个性化PIN |
| 43 | network subset personalization PUK required | 需要网络子集个性化PUK |
| 44 | service provider personalization PIN required | 需要服务供应商个性化PIN |
| 45 | service provider personalization PUK required | 需要服务供应商个性化PUK |
| 46 | corporate personalization PIN required | 需要公司个性化PIN |
| 47 | corporate personalization PUK required | 需要公司个性化PUK |
| 48 | hidden key required | 需要输入隐藏的密码 |
| 49 | EXE_NOT_SURPORT | |
| 50 | EXE_FAIL | (Cat.1) |
| 50 | Invalid Param | 无效参数 (Cat.4) |
| 51 | NO MEMORY | 内存不足 (Cat.1) |
| 52 | OPTION NOT SURPORT | 选项不支持 (Cat.1) |
| 53 | parameters are invalid | 无效参数 (Cat.1) |
| 54 | EXT_REG_NOT_EXIT | (Cat.1) |
| 55 | EXT_SMS_NOT_EXIT | (Cat.1) |
| 56 | EXT_PBK_NOT_EXIT | (Cat.1) |
| 57 | EXT_FFS_NOT_EXIT | (Cat.1) |
| 58 | INVALID_COMMAND_LINE | (Cat.1) |
| 59 | ITF_DIFFERENT | (Cat.1) |
| 60 | BURN_FLASH_FAIL | (Cat.1) |
| 61 | TFLASH NOT EXIST | TF卡不存在 (Cat.1) |
| 62 | FILE NOT EXIST | 文件不存在 (Cat.1) |
| 63 | FILE TOO LARGE | 文件太大 (Cat.1) |
| 96 | INVALID DATE OR TIME | 无效日期或时间 (Cat.1) |
| 97 | DIR CREATE FAIL | 创建文件夹失败 (Cat.1) |
| 98 | DIR NOT EXIST | 文件夹不存在 (Cat.1) |
| 99 | NOT IMPLEMENTED | 不可执行 (Cat.1) |
| 100 | unknown | 未知 |

## 网络与 GPRS 错误 (103–181)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 103 | Illegal MS | 非法MS |
| 106 | Illegal ME | 非法ME |
| 107 | GPRS services not allowed | 不允许GPRS业务 |
| 111 | PLMN not allowed | 不允许PLMN |
| 112 | Location area not allowed | 不允许位置区 |
| 113 | Roaming not allowed in this location area | 该位置区不允许漫游 |
| 132 | service option not supported | 不支持业务选择 |
| 133 | requested service option not subscribed | 未描述业务选择请求 |
| 134 | service option temporarily out of order | 业务选择暂时无连接 |
| 148 | unspecified GPRS error | GPRS错误未指明 |
| 149 | PDP authentication failure | PDP 鉴权失败 |
| 150 | invalid mobile class | 移动类别无效 |
| 151 | AT command timeout | AT命令超时 |
| 181 | UNSUPPORTED QCI VALUE | 不支持CQI |

## SS (补充业务) 错误 (214–285)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 214 | SS_UNKNOWN_SUBSCRIBER | |
| 222 | SS_ILLEGAL_SUBSCRIBER | |
| 223 | SS_BRERSERV_NOT_PROV | |
| 224 | SS_TELESERV_NOT_PROV | |
| 225 | SS_ILLEGAL_EQUIPMENT | |
| 226 | SS_CALL_BARRED | |
| 229 | SS_ILLEGAL_OPERATION | |
| 230 | SS_ERROR_STATUS | |
| 231 | SS_NOT_AVAILABLE | |
| 232 | SS_SUBS_VIOLATION | |
| 233 | SS_INCOMPATIBILITY | |
| 234 | SS_FACILITY_NOT_SUPPORTED | |
| 240 | SS_ABSENT_SUBSCRIBER | |
| 247 | SS_SYSTEM_FAILURE | |
| 248 | SS_DATA_MISSING | |
| 249 | SS_UNEXPECTED_DATA_VALUE | |
| 250 | SS_PWD_REGISTRATION_FAILURE | |
| 251 | SS_NEGATIVE_PWD_CHECK | |
| 256 | SS_NUMOF_PWD_ATTEMPT_VIOL | |
| 284 | SS_UNKNOWN_ALPHABET | |
| 285 | SS_USSD_BUSY | |

## SIM 相关错误 (264–283, Cat.1)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 264 | SIM VERIFY FAIL | (Cat.1) |
| 265 | SIM UNBLOCK FAIL | (Cat.1) |
| 266 | SIM CONDITION NO FULLFILLED | (Cat.1) |
| 267 | SIM UNBLOCK FAIL NO LEFT | (Cat.1) |
| 268 | SIM VERIFY FAIL NO LEFT | (Cat.1) |
| 269 | SIM INVALID PARAMETER | (Cat.1) |
| 270 | SIM UNKNOW COMMAND | (Cat.1) |
| 271 | SIM WRONG CLASS | (Cat.1) |
| 272 | SIM TECHNICAL PROBLEM | (Cat.1) |
| 273 | SIM CHV NEED UNBLOCK | (Cat.1) |
| 274 | SIM NOEF SELECTED | (Cat.1) |
| 275 | SIM FILE UNMATCH COMMAND | (Cat.1) |
| 276 | SIM CONTRADICTION CHV | (Cat.1) |
| 277 | SIM CONTRADICTION INVALIDATION | (Cat.1) |
| 278 | SIM MAXVALUE REACHED | (Cat.1) |
| 279 | SIM PATTERN NOT FOUND | (Cat.1) |
| 280 | SIM FILEID NOT FOUND | (Cat.1) |
| 281 | SIM STK BUSY | (Cat.1) |
| 282 | SIM UNKNOW | (Cat.1) |
| 283 | SIM PROFILE ERROR | (Cat.1) |

## 其他网络/呼叫错误 (267–774)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 267 | SS_POSITION_METHOD_FAILURE | (Cat.4) |
| 323 | — | Cat.1 模块 |
| 339 | SS_MAXMPTY_CALLS_EXCEEDED | |
| 340 | SS_RESOURCES_NOT_AVAILABLE | |
| 501 | WIFI labtool reture error | |
| 502 | BT labtool reture error | |
| 503 | FM labtool reture error | |
| 504 | MRD file already exist | |
| 505 | MRD file with same version already exist | |
| 506 | MRD file with newer version already exist | |
| 507 | MRD authorization failure | |
| 508 | (U)SIM PUK blocked | |
| 509 | Vendor not supported | |
| 510 | NVM path not exist | |
| 511 | NVM file comcfg error | |
| 535 | PROTOCOL stack busy | |
| 600 | BTSAP card not accessible | |
| 601 | BTSAP card powered off | |
| 602 | BTSAP card removed | |
| 603 | BTSAP card powered on | |
| 604 | BTSAP data not available | |
| 605 | BTSAP not supported | |
| 606 | Non-Production mode | |
| 753 | missing required cmd parameter | CRSM 缺少参数 |
| 754 | Invalid SIM command | CRSM 无效命令 |
| 755 | Invalid file id | CRSM 无效的文件 |
| 756 | Missing required P1/2/3 parameter | CRSM 缺少P 参数 |
| 757 | Invalid P1/2/3 parameter | CRSM 无效的P 参数 |
| 758 | Missing required command data | CRSM 缺少命令数据 |
| 759 | invalid characters in command data | CRSM 命令行中有无效字符 |
| 765 | Invalid input value | 无效输入值 |
| 766 | Unsupported mode | 不支持的模式 |
| 767 | Operation failed | 操作失败 |
| 768 | Mux already running | 多路复用已经在运行 |
| 769 | Unable to get control | 不能获得控制权 |
| 770 | SIM network reject | SIM 网络拒绝 |
| 771 | Call setup in progress | 正在建立呼叫 |
| 772 | SIM powered down | SIM 关闭了 |
| 773 | SIM file not present | SIM 文件不在 |
| 774 | RAC refresh net time failure | |

## 参数错误 (791–797)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 791 | Param count not enough | |
| 792 | Param count beyond | |
| 793 | Param value range beyond | |
| 794 | Param type not match | |
| 795 | Param format invalid | |
| 796 | Get a null param | |
| 797 | CFUN state is 0 or 4 | |

## AT 引擎内部错误 (810–824)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 810 | No Error | |
| 811 | Unrecognized Command | |
| 812 | Return Value Error | |
| 813 | Syntax Error | |
| 814 | Unspecified Error | |
| 815 | Data Transfer Already | |
| 816 | Action Already | |
| 817 | Not At Cmd | |
| 818 | Multi Cmd too long | |
| 819 | Abort Cops | |
| 820 | No Call Disc | |
| 821 | BT SAP Undefined | |
| 822 | BT SAP Not Accessible | |
| 823 | BT SAP Card Removed | |
| 824 | AT Not Allowed By Customer | |

## GPS 错误 (890–894)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 890 | GPS_NOT_RUNNING | |
| 891 | GPS_IS_RUNNING | |
| 892 | GPS_IS_FIXING | |
| 893 | GPS_IS_SLEEPING | |
| 894 | GPS_NOT_SLEEPING | |

## 数据 / PDP / TCP 错误 (900–918)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 900 | DIAED_REJECT | |
| 901 | PDP_NO_ACTIVE | |
| 902 | PDP_ACTIVE | |
| 910 | TCP_CONNECTION_REJECT | |
| 911 | TCP_CONNECT_OVERTIME | |
| 912 | SOCKET_CONNECTION_EXIST | |
| 913 | SOCKET_CONNECTION_NOT_EXIST | |
| 914 | BUFFER_OVER_SIZE | |
| 915 | SENDING_OVERTIME | |
| 916 | DNS_EXIST | |
| 917 | DNS_PARSE_OVERTIME | |
| 918 | DNS_PARSE_ERROR | |

## 通用输入错误 (980–983)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 980 | INPUT_VALUE_ERROR | |
| 981 | OTHER_ERROR | |
| 982 | ERROR | |
| 983 | NOT_ALLOWED | |

## 固件升级错误 (1000–1006)

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 1000 | UPGRADE_INVALID_URL | |
| 1001 | UPGRADE_NET_ERROR | |
| 1002 | UPGRADE_SERVER_CONNECT_ERROR | |
| 1003 | UPGRADE_INVALID_FILE | |
| 1004 | UPGRADE_SERVER_RESPONSE_ERROR | |
| 1005 | UPGRADE_WRITE_FLASH_ERROR | |
| 1006 | UPGRADE_ERROR | |

## 其它

| 错误码 | 冗长码 | 解释 |
|:------:|--------|------|
| 65535 | Other Error | |

---

*数据来源：[合宙文档中心 — CME ERROR (air780ep)](https://docs.openluat.com/air780ep/at/app/Command_List/Configuration/CME_ERROR/)*
