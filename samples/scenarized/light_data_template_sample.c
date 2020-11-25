/*
 * Tencent is pleased to support the open source community by making IoT Hub
 available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file
 except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software
 distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 KIND,
 * either express or implied. See the License for the specific language
 governing permissions and
 * limitations under the License.
 *
 */

#include "lite-utils.h"
#include "qcloud_iot_export.h"
#include "qcloud_iot_import.h"
#include "utils_timer.h"

#ifdef AUTH_MODE_CERT
static char sg_cert_file[PATH_MAX + 1];  // full path of device cert file
static char sg_key_file[PATH_MAX + 1];   // full path of device key file
#endif

static DeviceInfo sg_devInfo;
static Timer      sg_reportTimer;

/* anis color control codes */
#define ANSI_COLOR_RED    "\x1b[31m"
#define ANSI_COLOR_GREEN  "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE   "\x1b[34m"
#define ANSI_COLOR_RESET  "\x1b[0m"

static MQTTEventType sg_subscribe_event_result = MQTT_EVENT_UNDEF;
static bool          sg_control_msg_arrived    = false;
static char          sg_data_report_buffer[2048];
static size_t        sg_data_report_buffersize = sizeof(sg_data_report_buffer) / sizeof(sg_data_report_buffer[0]);

/*data_config.c can be generated by tools/codegen.py -c xx/product.json*/
/*-----------------data config start  -------------------*/
#define TOTAL_PROPERTY_COUNT 4
#define MAX_STR_NAME_LEN     (64)

static sDataPoint sg_DataTemplate[TOTAL_PROPERTY_COUNT];

typedef enum {
    eCOLOR_RED   = 0,
    eCOLOR_GREEN = 1,
    eCOLOR_BLUE  = 2,
} eColor;

typedef struct _ProductDataDefine {
    TYPE_DEF_TEMPLATE_BOOL   m_light_switch;
    TYPE_DEF_TEMPLATE_ENUM   m_color;
    TYPE_DEF_TEMPLATE_INT    m_brightness;
    TYPE_DEF_TEMPLATE_STRING m_name[MAX_STR_NAME_LEN + 1];
} ProductDataDefine;

static ProductDataDefine sg_ProductData;

static void _init_data_template(void)
{
    memset((void *)&sg_ProductData, 0, sizeof(ProductDataDefine));

    sg_ProductData.m_light_switch         = 0;
    sg_DataTemplate[0].data_property.key  = "power_switch";
    sg_DataTemplate[0].data_property.data = &sg_ProductData.m_light_switch;
    sg_DataTemplate[0].data_property.type = TYPE_TEMPLATE_BOOL;

    sg_ProductData.m_color                = eCOLOR_RED;
    sg_DataTemplate[1].data_property.key  = "color";
    sg_DataTemplate[1].data_property.data = &sg_ProductData.m_color;
    sg_DataTemplate[1].data_property.type = TYPE_TEMPLATE_ENUM;

    sg_ProductData.m_brightness           = 0;
    sg_DataTemplate[2].data_property.key  = "brightness";
    sg_DataTemplate[2].data_property.data = &sg_ProductData.m_brightness;
    sg_DataTemplate[2].data_property.type = TYPE_TEMPLATE_INT;

    strncpy(sg_ProductData.m_name, sg_devInfo.device_name, MAX_STR_NAME_LEN);
    sg_ProductData.m_name[strlen(sg_devInfo.device_name)] = '\0';
    sg_DataTemplate[3].data_property.key                  = "name";
    sg_DataTemplate[3].data_property.data                 = sg_ProductData.m_name;
    sg_DataTemplate[3].data_property.data_buff_len        = MAX_STR_NAME_LEN;
    sg_DataTemplate[3].data_property.type                 = TYPE_TEMPLATE_STRING;
};
/*-----------------data config end  -------------------*/

/*event_config.c can be generated by tools/codegen.py -c xx/product.json*/
/*-----------------event config start  -------------------*/
#ifdef EVENT_POST_ENABLED
#define EVENT_COUNTS              (3)
#define MAX_EVENT_STR_MESSAGE_LEN (64)
#define MAX_EVENT_STR_NAME_LEN    (64)

static TYPE_DEF_TEMPLATE_BOOL   sg_status;
static TYPE_DEF_TEMPLATE_STRING sg_message[MAX_EVENT_STR_MESSAGE_LEN + 1];
static DeviceProperty           g_propertyEvent_status_report[] = {

    {.key = "status", .data = &sg_status, .type = TYPE_TEMPLATE_BOOL},
    {.key = "message", .data = sg_message, .type = TYPE_TEMPLATE_STRING},
};

static TYPE_DEF_TEMPLATE_FLOAT sg_voltage;
static DeviceProperty          g_propertyEvent_low_voltage[] = {

    {.key = "voltage", .data = &sg_voltage, .type = TYPE_TEMPLATE_FLOAT},
};

static TYPE_DEF_TEMPLATE_STRING sg_name[MAX_EVENT_STR_NAME_LEN + 1];
static TYPE_DEF_TEMPLATE_INT    sg_error_code;
static DeviceProperty           g_propertyEvent_hardware_fault[] = {

    {.key = "name", .data = sg_name, .type = TYPE_TEMPLATE_STRING},
    {.key = "error_code", .data = &sg_error_code, .type = TYPE_TEMPLATE_INT},
};

static sEvent g_events[] = {

    {
        .event_name   = "status_report",
        .type         = "info",
        .timestamp    = 0,
        .eventDataNum = sizeof(g_propertyEvent_status_report) / sizeof(g_propertyEvent_status_report[0]),
        .pEventData   = g_propertyEvent_status_report,
    },
    {
        .event_name   = "low_voltage",
        .type         = "alert",
        .timestamp    = 0,
        .eventDataNum = sizeof(g_propertyEvent_low_voltage) / sizeof(g_propertyEvent_low_voltage[0]),
        .pEventData   = g_propertyEvent_low_voltage,
    },
    {
        .event_name   = "hardware_fault",
        .type         = "fault",
        .timestamp    = 0,
        .eventDataNum = sizeof(g_propertyEvent_hardware_fault) / sizeof(g_propertyEvent_hardware_fault[0]),
        .pEventData   = g_propertyEvent_hardware_fault,
    },
};

/*-----------------event config end -------------------*/

static void update_events_timestamp(sEvent *pEvents, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (NULL == (&pEvents[i])) {
            Log_e("null event pointer");
            return;
        }
#ifdef EVENT_TIMESTAMP_USED
        pEvents[i].timestamp = HAL_Timer_current_sec();  // should be UTC and
        // accurate
#else
        pEvents[i].timestamp = 0;
#endif
    }
}

static void event_post_cb(void *pClient, MQTTMessage *msg)
{
    Log_d("recv event reply, clear event");
    //    IOT_Event_clearFlag(pClient, FLAG_EVENT0);
}

// event check and post
static void eventPostCheck(void *client)
{
    int      i;
    int      rc;
    uint32_t eflag;
    uint8_t  EventCont;
    sEvent * pEventList[EVENT_COUNTS];

    eflag = IOT_Event_getFlag(client);
    if ((EVENT_COUNTS > 0) && (eflag > 0)) {
        EventCont = 0;
        for (i = 0; i < EVENT_COUNTS; i++) {
            if ((eflag & (1 << i)) & ALL_EVENTS_MASK) {
                pEventList[EventCont++] = &(g_events[i]);
                update_events_timestamp(&g_events[i], 1);
                IOT_Event_clearFlag(client, (1 << i) & ALL_EVENTS_MASK);
            }
        }

        rc = IOT_Post_Event(client, sg_data_report_buffer, sg_data_report_buffersize, EventCont, pEventList,
                            event_post_cb);
        if (rc < 0) {
            Log_e("event post failed: %d", rc);
        }
    }
}

#endif

/*action_config.c can be generated by tools/codegen.py -c xx/product.json*/
/*-----------------action config start  -------------------*/
#ifdef ACTION_ENABLED

#define TOTAL_ACTION_COUNTS (1)

static TYPE_DEF_TEMPLATE_INT sg_blink_in_period    = 5;
static DeviceProperty        g_actionInput_blink[] = {
    {.key = "period", .data = &sg_blink_in_period, .type = TYPE_TEMPLATE_INT}
};
static TYPE_DEF_TEMPLATE_BOOL sg_blink_out_result    = 0;
static DeviceProperty         g_actionOutput_blink[] = {

    {.key = "result", .data = &sg_blink_out_result, .type = TYPE_TEMPLATE_BOOL},
};

static DeviceAction g_actions[] = {

    {
        .pActionId  = "blink",
        .timestamp  = 0,
        .input_num  = sizeof(g_actionInput_blink) / sizeof(g_actionInput_blink[0]),
        .output_num = sizeof(g_actionOutput_blink) / sizeof(g_actionOutput_blink[0]),
        .pInput     = g_actionInput_blink,
        .pOutput    = g_actionOutput_blink,
    },
};
/*-----------------action config end    -------------------*/
static void OnActionCallback(void *pClient, const char *pClientToken, DeviceAction *pAction)
{
    int        i;
    sReplyPara replyPara;

    // control light blink
    int             period       = 0;
    DeviceProperty *pActionInput = pAction->pInput;
    for (i = 0; i < pAction->input_num; i++) {
        if (!strcmp(pActionInput[i].key, "period")) {
            period = *((int *)pActionInput[i].data);
        } else {
            Log_e("no such input[%s]!", pActionInput[i].key);
        }
    }

    // do blink
    HAL_Printf("%s[lighting blink][****]" ANSI_COLOR_RESET, ANSI_COLOR_RED);
    HAL_SleepMs(period * 1000);
    HAL_Printf("\r%s[lighting blink][****]" ANSI_COLOR_RESET, ANSI_COLOR_GREEN);
    HAL_SleepMs(period * 1000);
    HAL_Printf("\r%s[lighting blink][****]\n" ANSI_COLOR_RESET, ANSI_COLOR_RED);

    // construct output
    memset((char *)&replyPara, 0, sizeof(sReplyPara));
    replyPara.code       = eDEAL_SUCCESS;
    replyPara.timeout_ms = QCLOUD_IOT_MQTT_COMMAND_TIMEOUT;
    strcpy(replyPara.status_msg,
           "action execute success!");  // add the message about the action resault

    DeviceProperty *pActionOutnput   = pAction->pOutput;
    *(int *)(pActionOutnput[0].data) = 0;  // set result

    IOT_Action_Reply(pClient, pClientToken, sg_data_report_buffer, sg_data_report_buffersize, pAction, &replyPara);
}

static int _register_data_template_action(void *pTemplate_client)
{
    int i, rc;

    for (i = 0; i < TOTAL_ACTION_COUNTS; i++) {
        rc = IOT_Template_Register_Action(pTemplate_client, &g_actions[i], OnActionCallback);
        if (rc != QCLOUD_RET_SUCCESS) {
            rc = IOT_Template_Destroy(pTemplate_client);
            Log_e("register device data template action failed, err: %d", rc);
            return rc;
        } else {
            Log_i("data template action=%s registered.", g_actions[i].pActionId);
        }
    }

    return QCLOUD_RET_SUCCESS;
}
#endif

static void event_handler(void *pclient, void *handle_context, MQTTEventMsg *msg)
{
    uintptr_t packet_id = (uintptr_t)msg->msg;

    switch (msg->event_type) {
        case MQTT_EVENT_UNDEF:
            Log_i("undefined event occur.");
            break;

        case MQTT_EVENT_DISCONNECT:
            Log_i("MQTT disconnect.");
            break;

        case MQTT_EVENT_RECONNECT:
            Log_i("MQTT reconnect.");
            break;

        case MQTT_EVENT_SUBCRIBE_SUCCESS:
            sg_subscribe_event_result = msg->event_type;
            Log_i("subscribe success, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_SUBCRIBE_TIMEOUT:
            sg_subscribe_event_result = msg->event_type;
            Log_i("subscribe wait ack timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_SUBCRIBE_NACK:
            sg_subscribe_event_result = msg->event_type;
            Log_i("subscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_PUBLISH_SUCCESS:
            Log_i("publish success, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_PUBLISH_TIMEOUT:
            Log_i("publish timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case MQTT_EVENT_PUBLISH_NACK:
            Log_i("publish nack, packet-id=%u", (unsigned int)packet_id);
            break;
        default:
            Log_i("Should NOT arrive here.");
            break;
    }
}

/*add user init code, like sensor init*/
static void _usr_init(void)
{
    Log_d("add your init code here");
}

// Setup MQTT construct parameters
static int _setup_connect_init_params(TemplateInitParams *initParams)
{
    int ret;

    ret = HAL_GetDevInfo((void *)&sg_devInfo);
    if (QCLOUD_RET_SUCCESS != ret) {
        return ret;
    }

    initParams->region      = sg_devInfo.region;
    initParams->device_name = sg_devInfo.device_name;
    initParams->product_id  = sg_devInfo.product_id;

#ifdef AUTH_MODE_CERT
    /* TLS with certs*/
    char  certs_dir[PATH_MAX + 1] = "certs";
    char  current_path[PATH_MAX + 1];
    char *cwd = getcwd(current_path, sizeof(current_path));
    if (cwd == NULL) {
        Log_e("getcwd return NULL");
        return QCLOUD_ERR_FAILURE;
    }
    sprintf(sg_cert_file, "%s/%s/%s", current_path, certs_dir, sg_devInfo.dev_cert_file_name);
    sprintf(sg_key_file, "%s/%s/%s", current_path, certs_dir, sg_devInfo.dev_key_file_name);

    initParams->cert_file = sg_cert_file;
    initParams->key_file  = sg_key_file;
#else
    initParams->device_secret = sg_devInfo.device_secret;
#endif

    initParams->command_timeout        = QCLOUD_IOT_MQTT_COMMAND_TIMEOUT;
    initParams->keep_alive_interval_ms = QCLOUD_IOT_MQTT_KEEP_ALIVE_INTERNAL;
    initParams->auto_connect_enable    = 1;
    initParams->event_handle.h_fp      = event_handler;

    return QCLOUD_RET_SUCCESS;
}

#ifdef LOG_UPLOAD
// init log upload module
static int _init_log_upload(TemplateInitParams *init_params)
{
    LogUploadInitParams log_init_params;
    memset(&log_init_params, 0, sizeof(LogUploadInitParams));

    log_init_params.region = init_params->region;
    log_init_params.product_id = init_params->product_id;
    log_init_params.device_name = init_params->device_name;
#ifdef AUTH_MODE_CERT
    log_init_params.sign_key = init_params->cert_file;
#else
    log_init_params.sign_key = init_params->device_secret;
#endif

#if defined(__linux__) || defined(WIN32)
    log_init_params.read_func = HAL_Log_Read;
    log_init_params.save_func = HAL_Log_Save;
    log_init_params.del_func = HAL_Log_Del;
    log_init_params.get_size_func = HAL_Log_Get_Size;
#endif

    return IOT_Log_Init_Uploader(&log_init_params);
}
#endif

/*control msg from server will trigger this callback*/
static void OnControlMsgCallback(void *pClient, const char *pJsonValueBuffer, uint32_t valueLength,
                                 DeviceProperty *pProperty)
{
    int i = 0;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        /* handle self defined string/json here. Other properties are dealed in
         * _handle_delta()*/
        if (strcmp(sg_DataTemplate[i].data_property.key, pProperty->key) == 0) {
            sg_DataTemplate[i].state = eCHANGED;
            Log_i("Property=%s changed", pProperty->key);
            sg_control_msg_arrived = true;
            return;
        }
    }

    Log_e("Property=%s changed no match", pProperty->key);
}

static void OnReportReplyCallback(void *pClient, Method method, ReplyAck replyAck, const char *pJsonDocument,
                                  void *pUserdata)
{
    Log_i("recv report_reply(ack=%d): %s", replyAck, pJsonDocument);
}

// register data template properties
static int _register_data_template_property(void *pTemplate_client)
{
    int i, rc;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        rc = IOT_Template_Register_Property(pTemplate_client, &sg_DataTemplate[i].data_property, OnControlMsgCallback);
        if (rc != QCLOUD_RET_SUCCESS) {
            rc = IOT_Template_Destroy(pTemplate_client);
            Log_e("register device data template property failed, err: %d", rc);
            return rc;
        } else {
            Log_i("data template property=%s registered.", sg_DataTemplate[i].data_property.key);
        }
    }

    return QCLOUD_RET_SUCCESS;
}

/*get property state, changed or not*/
static eDataState get_property_state(void *pProperyData)
{
    int i;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        if (sg_DataTemplate[i].data_property.data == pProperyData) {
            return sg_DataTemplate[i].state;
        }
    }

    Log_e("no property matched");
    return eNOCHANGE;
}

/*set property state, changed or no change*/
static void set_property_state(void *pProperyData, eDataState state)
{
    int i;

    for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        if (sg_DataTemplate[i].data_property.data == pProperyData) {
            sg_DataTemplate[i].state = state;
            break;
        }
    }
}

/* demo for light logic deal */
static void deal_down_stream_user_logic(void *client, ProductDataDefine *light)
{
    int         i;
    const char *ansi_color         = NULL;
    const char *ansi_color_name    = NULL;
    char        brightness_bar[]   = "||||||||||||||||||||";
    int         brightness_bar_len = strlen(brightness_bar);

    /* light color */
    switch (light->m_color) {
        case eCOLOR_RED:
            ansi_color      = ANSI_COLOR_RED;
            ansi_color_name = " RED ";
            break;
        case eCOLOR_GREEN:
            ansi_color      = ANSI_COLOR_GREEN;
            ansi_color_name = "GREEN";
            break;
        case eCOLOR_BLUE:
            ansi_color      = ANSI_COLOR_BLUE;
            ansi_color_name = " BLUE";
            break;
        default:
            ansi_color      = ANSI_COLOR_YELLOW;
            ansi_color_name = "UNKNOWN";
            break;
    }

    /* light brightness bar */
    brightness_bar_len =
        (light->m_brightness >= 100) ? brightness_bar_len : (int)((light->m_brightness * brightness_bar_len) / 100);
    for (i = brightness_bar_len; i < strlen(brightness_bar); i++) {
        brightness_bar[i] = '-';
    }

    if (light->m_light_switch) {
        /* light is on , show with the properties*/
        HAL_Printf("%s[  lighting  ]|[color:%s]|[brightness:%s]|[%s]\n" ANSI_COLOR_RESET, ansi_color, ansi_color_name,
                   brightness_bar, light->m_name);
    } else {
        /* light is off */
        HAL_Printf(ANSI_COLOR_YELLOW "[  light is off ]|[color:%s]|[brightness:%s]|[%s]\n" ANSI_COLOR_RESET,
                   ansi_color_name, brightness_bar, light->m_name);
    }

    if (eCHANGED == get_property_state(&light->m_light_switch)) {
#ifdef EVENT_POST_ENABLED
        if (light->m_light_switch) {
            *(TYPE_DEF_TEMPLATE_BOOL *)g_events[0].pEventData[0].data = 1;
            memset((TYPE_DEF_TEMPLATE_STRING *)g_events[0].pEventData[1].data, 0, MAX_EVENT_STR_MESSAGE_LEN);
            strcpy((TYPE_DEF_TEMPLATE_STRING *)g_events[0].pEventData[1].data, "light on");
        } else {
            *(TYPE_DEF_TEMPLATE_BOOL *)g_events[0].pEventData[0].data = 0;
            memset((TYPE_DEF_TEMPLATE_STRING *)g_events[0].pEventData[1].data, 0, MAX_EVENT_STR_MESSAGE_LEN);
            strcpy((TYPE_DEF_TEMPLATE_STRING *)g_events[0].pEventData[1].data, "light off");
        }

        // switch state changed set EVENT0 flag, the events will be posted by
        // eventPostCheck
        IOT_Event_setFlag(client, FLAG_EVENT0);
#else
        Log_d("light switch state changed");
#endif
    }
}

/*example for cycle report, you can delete this for your needs*/
static void cycle_report(Timer *reportTimer)
{
    int i;

    if (expired(reportTimer)) {
        for (i = 0; i < TOTAL_PROPERTY_COUNT; i++) {
            set_property_state(sg_DataTemplate[i].data_property.data, eCHANGED);
            countdown_ms(reportTimer, 5000);
        }
    }
}

/*get local property data, like sensor data*/
static void _refresh_local_property(void)
{
    // add your local property refresh logic, cycle report for example
    cycle_report(&sg_reportTimer);
}

/*find propery need report*/
static int find_wait_report_property(DeviceProperty *pReportDataList[])
{
    int i, j;

    for (i = 0, j = 0; i < TOTAL_PROPERTY_COUNT; i++) {
        if (eCHANGED == sg_DataTemplate[i].state) {
            pReportDataList[j++]     = &(sg_DataTemplate[i].data_property);
            sg_DataTemplate[i].state = eNOCHANGE;
        }
    }

    return j;
}

/* demo for up-stream code */
static int deal_up_stream_user_logic(DeviceProperty *pReportDataList[], int *pCount)
{
    // refresh local property
    _refresh_local_property();

    /*find propery need report*/
    *pCount = find_wait_report_property(pReportDataList);

    return (*pCount > 0) ? QCLOUD_RET_SUCCESS : QCLOUD_ERR_FAILURE;
}

/*You should get the real info for your device, here just for example*/
static int _get_sys_info(void *handle, char *pJsonDoc, size_t sizeOfBuffer)
{
    /*platform info has at least one of module_hardinfo/module_softinfo/fw_ver
     * property*/
    DeviceProperty plat_info[] = {
        {.key = "module_hardinfo", .type = TYPE_TEMPLATE_STRING, .data = "ESP8266"},
        {.key = "module_softinfo", .type = TYPE_TEMPLATE_STRING, .data = "V1.0"},
        {.key = "fw_ver", .type = TYPE_TEMPLATE_STRING, .data = QCLOUD_IOT_DEVICE_SDK_VERSION},
        {.key = "imei", .type = TYPE_TEMPLATE_STRING, .data = "11-22-33-44"},
        {.key = "lat", .type = TYPE_TEMPLATE_STRING, .data = "22.546015"},
        {.key = "lon", .type = TYPE_TEMPLATE_STRING, .data = "113.941125"},
        {NULL, NULL, 0}  // end
    };

    /*self define info*/
    DeviceProperty self_info[] = {
        {.key = "append_info", .type = TYPE_TEMPLATE_STRING, .data = "your self define info"}, {NULL, NULL, 0}  // end
    };

    return IOT_Template_JSON_ConstructSysInfo(handle, pJsonDoc, sizeOfBuffer, plat_info, self_info);
}

int main(int argc, char **argv)
{
    DeviceProperty *pReportDataList[TOTAL_PROPERTY_COUNT];
    sReplyPara      replyPara;
    int             ReportCont;
    int             rc;

    // init log level
    IOT_Log_Set_Level(eLOG_DEBUG);

    // init connection
    TemplateInitParams init_params = DEFAULT_TEMPLATE_INIT_PARAMS;
    rc                             = _setup_connect_init_params(&init_params);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("init params err,rc=%d", rc);
        return rc;
    }

#ifdef LOG_UPLOAD
    // _init_log_upload should be done after _setup_connect_init_params and before IOT_Template_Construct
    rc = _init_log_upload(&init_params);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("init log upload error, rc = %d", rc);
    }
#endif

    void *client = IOT_Template_Construct(&init_params, NULL);
    if (client != NULL) {
        Log_i("Cloud Device Construct Success");
    } else {
        Log_e("Cloud Device Construct Failed");
        return QCLOUD_ERR_FAILURE;
    }

#ifdef MULTITHREAD_ENABLED
    if (QCLOUD_RET_SUCCESS != IOT_Template_Start_Yield_Thread(client)) {
        Log_e("start template yield thread fail");
        goto exit;
    }
#endif

    // usr init
    _usr_init();

    // init data template
    _init_data_template();

    // register data template propertys here
    rc = _register_data_template_property(client);
    if (rc == QCLOUD_RET_SUCCESS) {
        Log_i("Register data template propertys Success");
    } else {
        Log_e("Register data template propertys Failed: %d", rc);
        goto exit;
    }

// register data template actions here
#ifdef ACTION_ENABLED
    rc = _register_data_template_action(client);
    if (rc == QCLOUD_RET_SUCCESS) {
        Log_i("Register data template actions Success");
    } else {
        Log_e("Register data template actions Failed: %d", rc);
        goto exit;
    }
#endif

    // report device info, then you can manager your product by these info, like position
    rc = _get_sys_info(client, sg_data_report_buffer, sg_data_report_buffersize);
    if (QCLOUD_RET_SUCCESS == rc) {
        rc = IOT_Template_Report_SysInfo_Sync(client, sg_data_report_buffer, sg_data_report_buffersize,
                                              QCLOUD_IOT_MQTT_COMMAND_TIMEOUT);
        if (rc != QCLOUD_RET_SUCCESS) {
            Log_e("Report system info fail, err: %d", rc);
        }
    } else {
        Log_e("Get system info fail, err: %d", rc);
    }

    // get the property changed during offline
    rc = IOT_Template_GetStatus_sync(client, QCLOUD_IOT_MQTT_COMMAND_TIMEOUT);
    if (rc != QCLOUD_RET_SUCCESS) {
        Log_e("Get data status fail, err: %d", rc);
    } else {
        Log_d("Get data status success");
    }

    // init a timer for cycle report, you could delete it or not for your needs
    InitTimer(&sg_reportTimer);

    while (IOT_Template_IsConnected(client) || rc == QCLOUD_ERR_MQTT_ATTEMPTING_RECONNECT ||
           rc == QCLOUD_RET_MQTT_RECONNECTED || QCLOUD_RET_SUCCESS == rc) {
#ifndef MULTITHREAD_ENABLED
        rc = IOT_Template_Yield(client, 200);
        if (rc == QCLOUD_ERR_MQTT_ATTEMPTING_RECONNECT) {
            HAL_SleepMs(1000);
            continue;
        } else if (rc != QCLOUD_RET_SUCCESS) {
            Log_e("Exit loop caused of errCode: %d", rc);
        }
#endif
        /* handle control msg from server */
        if (sg_control_msg_arrived) {
            deal_down_stream_user_logic(client, &sg_ProductData);
            /* control msg should reply, otherwise server treat device didn't receive
             * and retain the msg which would be get by get status*/
            memset((char *)&replyPara, 0, sizeof(sReplyPara));
            replyPara.code          = eDEAL_SUCCESS;
            replyPara.timeout_ms    = QCLOUD_IOT_MQTT_COMMAND_TIMEOUT;
            replyPara.status_msg[0] = '\0';  // add extra info to replyPara.status_msg when error occured

            rc = IOT_Template_ControlReply(client, sg_data_report_buffer, sg_data_report_buffersize, &replyPara);
            if (rc == QCLOUD_RET_SUCCESS) {
                Log_d("Contol msg reply success");
                sg_control_msg_arrived = false;
            } else {
                Log_e("Contol msg reply failed, err: %d", rc);
            }
        }

        /*report msg to server*/
        /*report the lastest properties's status*/
        if (QCLOUD_RET_SUCCESS == deal_up_stream_user_logic(pReportDataList, &ReportCont)) {
            rc = IOT_Template_JSON_ConstructReportArray(client, sg_data_report_buffer, sg_data_report_buffersize,
                    ReportCont, pReportDataList);
            if (rc == QCLOUD_RET_SUCCESS) {
                rc = IOT_Template_Report(client, sg_data_report_buffer, sg_data_report_buffersize,
                                         OnReportReplyCallback, NULL, QCLOUD_IOT_MQTT_COMMAND_TIMEOUT);
                if (rc == QCLOUD_RET_SUCCESS) {
                    Log_i("data template reporte success");
                } else {
                    Log_e("data template reporte failed, err: %d", rc);
                }
            } else {
                Log_e("construct reporte data failed, err: %d", rc);
            }
        }

#ifdef EVENT_POST_ENABLED
        eventPostCheck(client);
#endif
        HAL_SleepMs(1000);
    }

exit:

#ifdef MULTITHREAD_ENABLED
    IOT_Template_Stop_Yield_Thread(client);
#endif
    rc = IOT_Template_Destroy(client);

#ifdef LOG_UPLOAD
    IOT_Log_Upload(true);
    IOT_Log_Fini_Uploader();
#endif

    return rc;
}
