//app
#include "chassis.h"
#include "robot_def.h"

//module
#include "dji_motor.h"
#include "super_cap.h"
#include "message_center.h"
#include "ins_task.h"
#include "general_def.h"
#include "buzzer.h"
#include "rm_referee.h"
#include "referee_task.h"
#include "Board2Board.h"
#include "super_cap.h"
//bsp
#include "bsp_dwt.h"
#include "arm_math.h"
/************************************** CommUsed **************************************/
/* 底盘应用包含的模块和信息存储,底盘是单例模式,因此不需要为底盘建立单独的结构体 */
static BoardCommInstance *chassis_can_comm;               // 双板通信CAN comm
static Chassis_Ctrl_Cmd_s chassis_cmd_recv;             // 底盘接收到的控制命令（发布中心发给底盘的）
static Chassis_Upload_Data_s chassis_feedback_data;     // 底盘回传的反馈数据

static Referee_Interactive_info_t ui_data;              // UI数据，将底盘中的数据传入此结构体的对应变量中，UI会自动检测是否变化，对应显示UI
static referee_info_t* referee_data;                    // 用于获取裁判系统的数据
static SuperCapInstance *cap;                                       // 超级电容
static uint16_t power_data;

/*********************************** CalculateSpeed ***********************************/
static DJIMotorInstance *motor_lf, *motor_rf, *motor_lb, *motor_rb; 
static float chassis_vx, chassis_vy;                    // 将云台系的速度投影到底盘
static float vt_lf, vt_rf, vt_lb, vt_rb;                // 底盘速度解算后的临时输出,跟据功率的多少再乘上一个系数
static float sin_theta, cos_theta;                      //麦轮解算用
static float vx,vy;                                     //获取车体信息要用到的中间变量
static float cnt=0;
static float rotate_buff;

void ChassisInit()
{
/***************************MOTOR_INIT******************************/
    // 底盘电机的初始化，包括什么通信、什么id、pid及电机安装是正装还是反装（相当于给最终输出值添负号）及型号
    // 四个轮子的参数一样,改tx_id和反转标志位即可
    Motor_Init_Config_s chassis_motor_config = {
        .can_init_config.can_handle = &hcan1,
        .controller_param_init_config = {
            .speed_PID = 
            {
                .Kp = 6, // 4.5
                .Ki = 0,  // 0
                .Kd = 0,  // 0
                .IntegralLimit = 3000,
                .Improve = PID_Trapezoid_Intergral | PID_Integral_Limit | PID_Derivative_On_Measurement,
                .MaxOut = 12000,
            },
            .current_PID = 
            {
                .Kp = 0.5, // 0.4
                .Ki = 0,   // 0
                .Kd = 0,
                .IntegralLimit = 3000,
                .Improve = PID_Trapezoid_Intergral | PID_Integral_Limit | PID_Derivative_On_Measurement,
                .MaxOut = 15000,
            },
        },

        .controller_setting_init_config = {
            .angle_feedback_source = MOTOR_FEED,
            .speed_feedback_source = MOTOR_FEED,
            .outer_loop_type = SPEED_LOOP,
            .close_loop_type = CURRENT_LOOP | SPEED_LOOP,
        },
        .motor_type = M3508,
    };
    //电机id号一定一定得一一对应
    chassis_motor_config.can_init_config.tx_id = 0x201;
    chassis_motor_config.controller_setting_init_config.motor_reverse_flag = MOTOR_DIRECTION_NORMAL;
    motor_lf = DJIMotorInit(&chassis_motor_config);
    
    chassis_motor_config.can_init_config.tx_id = 0x202;
    chassis_motor_config.controller_setting_init_config.motor_reverse_flag = MOTOR_DIRECTION_NORMAL;
    motor_lb = DJIMotorInit(&chassis_motor_config);

    chassis_motor_config.can_init_config.tx_id = 0x204;
    chassis_motor_config.controller_setting_init_config.motor_reverse_flag = MOTOR_DIRECTION_REVERSE;
    motor_rf = DJIMotorInit(&chassis_motor_config);

    chassis_motor_config.can_init_config.tx_id = 0x203;
    chassis_motor_config.controller_setting_init_config.motor_reverse_flag = MOTOR_DIRECTION_REVERSE;
    motor_rb = DJIMotorInit(&chassis_motor_config);

/************************************** RefereeCommInit **************************************/
    referee_data   = UITaskInit(&huart6,&ui_data);

/************************************** ChassisCommInit **************************************/
    //双板通信
    BoardComm_Init_Config_s comm_conf = {
        .can_config = {
            .can_handle = &hcan2,
            //云台的tx是底盘的rx，别搞错了！！！
            .tx_id = 0x209,
            .rx_id = 0x200,
        },
        .recv_data_len = sizeof(Chassis_Ctrl_Cmd_s),
        .send_data_len = sizeof(Chassis_Upload_Data_s),
    };
    chassis_can_comm = BoardCommInit(&comm_conf); // can comm初始化

    
    SuperCap_Init_Config_s capconfig = {
        .can_config = {
            .can_handle = &hcan1,
            .rx_id = 0x311,
            .tx_id = 0x310,
        },
        .recv_data_len = sizeof(int16_t),
        .send_data_len = sizeof(uint16_t),
    };
    cap=SuperCapInit(&capconfig);
}



/*****************************************MoveChassis********************************************/
static void ChassisStateSet()
{
    if (chassis_cmd_recv.chassis_mode == CHASSIS_ZERO_FORCE)
    { // 如果出现重要模块离线或遥控器设置为急停,让电机停止
        DJIMotorStop(motor_lf);
        DJIMotorStop(motor_rf);
        DJIMotorStop(motor_lb);
        DJIMotorStop(motor_rb);
    }
    else
    { // 正常工作
        DJIMotorEnable(motor_lf);
        DJIMotorEnable(motor_rf);
        DJIMotorEnable(motor_lb);
        DJIMotorEnable(motor_rb);
    }
}

/**
 * @brief  
 */
static void ChassisRotateSet()
{
    cnt = (float32_t)DWT_GetTimeline_s();//用于变速小陀螺
    switch (chassis_cmd_recv.chassis_mode)
    {
        case CHASSIS_NO_FOLLOW: // 底盘不旋转,但维持全向机动
            chassis_cmd_recv.wz = 0;
        break;

        case CHASSIS_ROTATE: // 变速小陀螺
            chassis_cmd_recv.wz = 10000*rotate_buff;
        break;
        case CHASSIS_NAV:
        chassis_cmd_recv.wz = (30000)*rotate_buff*chassis_cmd_recv.w/100;
        break;

        default:
        break;
    }
}

/**
 * @brief 计算每个底盘电机的输出,底盘正运动学解算
 *                                
 */
static void MecanumCalculate()
{   
    cos_theta = arm_cos_f32(chassis_cmd_recv.offset_angle * DEGREE_2_RAD);
    sin_theta = arm_sin_f32(chassis_cmd_recv.offset_angle * DEGREE_2_RAD);

    chassis_vx = chassis_cmd_recv.vx * cos_theta - chassis_cmd_recv.vy * sin_theta; 
    chassis_vy = chassis_cmd_recv.vx * sin_theta + chassis_cmd_recv.vy * cos_theta;

    vt_lf = chassis_vx - chassis_vy - chassis_cmd_recv.wz ;
    vt_lb = chassis_vx + chassis_vy - chassis_cmd_recv.wz ;
    vt_rb = chassis_vx - chassis_vy + chassis_cmd_recv.wz ;
    vt_rf = chassis_vx + chassis_vy + chassis_cmd_recv.wz ;
}

/**
 * @brief
 *
 */
static void ChassisOutput()
{ 
    DJIMotorSetRef(motor_lf, vt_lf);
    DJIMotorSetRef(motor_rf, vt_rf);
    DJIMotorSetRef(motor_lb, vt_lb);
    DJIMotorSetRef(motor_rb, vt_rb);
}

/*****************************************SendData********************************************/
/**
 * @brief 根据每个轮子的速度反馈,计算底盘的实际运动速度,逆运动解算，并发给巡航底盘实时数据             
 */
static void SendChassisData()
{
    //to 巡航
    vx = (motor_lf->measure.speed_aps +motor_lb->measure.speed_aps - motor_rb->measure.speed_aps - motor_rf->measure.speed_aps) / 4.0f / REDUCTION_RATIO_WHEEL / 360.0f * PERIMETER_WHEEL/1000 ;
    vy = (-motor_lf->measure.speed_aps +motor_lb->measure.speed_aps + motor_rb->measure.speed_aps - motor_rf->measure.speed_aps) / 4.0f / REDUCTION_RATIO_WHEEL / 360.0f * PERIMETER_WHEEL/1000  ;
    chassis_feedback_data.real_vx = vx * cos_theta + vy * sin_theta;
    chassis_feedback_data.real_vy = -vx * sin_theta + vy * cos_theta;
}




static void SendPowerData()
{
        power_data=referee_data->GameRobotState.chassis_power_limit;
        power_data=100;
        if(cap->cap_msg.vol<=24&&cap->cap_msg.vol>=18)
        {
            rotate_buff= 1.8;
        }
        //5 
        else if(cap->cap_msg.vol<18&&cap->cap_msg.vol>=14)
        {
            rotate_buff= 1.5;
        }
        //4 
        else if(cap->cap_msg.vol<14&&cap->cap_msg.vol>=12)
        {
            rotate_buff= 1;
        }
        else
        {
            rotate_buff= 0.5;
        }
}

/**
 * @brief  将裁判系统的信息发给巡航、视觉让其进行决策。
 */
static void SendJudgeData()
{
    chassis_feedback_data.Occupation=(referee_data->EventData.event_type >> 21) & 0x03;
    chassis_feedback_data.remain_time=referee_data->GameState.stage_remain_time;
    chassis_feedback_data.game_progress=referee_data->GameState.game_progress;
    if(referee_data->GameRobotState.robot_id>7)
    {
        chassis_feedback_data.enemy_color=COLOR_RED;

        chassis_feedback_data.remain_HP=referee_data->GameRobotHP.blue_7_robot_HP;
        chassis_feedback_data.self_hero_HP=referee_data->GameRobotHP.blue_1_robot_HP;
        chassis_feedback_data.self_infantry_HP=referee_data->GameRobotHP.blue_3_robot_HP;

        chassis_feedback_data.enemy_hero_HP=referee_data->GameRobotHP.red_1_robot_HP;
        chassis_feedback_data.enemy_infantry_HP=referee_data->GameRobotHP.red_3_robot_HP;
        chassis_feedback_data.enemy_sentry_HP=referee_data->GameRobotHP.red_7_robot_HP;
    }
    else
    {
        chassis_feedback_data.enemy_color=COLOR_BLUE;
        chassis_feedback_data.remain_HP=referee_data->GameRobotHP.red_7_robot_HP;
        chassis_feedback_data.self_hero_HP=referee_data->GameRobotHP.red_1_robot_HP;
        chassis_feedback_data.self_infantry_HP=referee_data->GameRobotHP.red_3_robot_HP;

        chassis_feedback_data.enemy_infantry_HP=referee_data->GameRobotHP.blue_1_robot_HP;
        chassis_feedback_data.enemy_infantry_HP=referee_data->GameRobotHP.blue_3_robot_HP;
        chassis_feedback_data.enemy_infantry_HP=referee_data->GameRobotHP.blue_7_robot_HP;
    }   
    chassis_feedback_data.left_bullet_heat= referee_data->PowerHeatData.shooter_17mm_2_barrel_heat;
    chassis_feedback_data.right_bullet_heat= referee_data->PowerHeatData.shooter_17mm_1_barrel_heat;
    chassis_feedback_data.bullet_num=750-referee_data->ProjectileAllowance.projectile_allowance_17mm;
    chassis_feedback_data.bullet_speed=referee_data->ShootData.bullet_speed;
}


/*********************************************************************************************************
 *********************************************      TASK     *********************************************
**********************************************************************************************************/
/* 机器人底盘控制核心任务 */
void ChassisTask()
{
/********************************************   GetRecvData  *********************************************/ 
    // 获取新的控制信息
    chassis_cmd_recv = *(Chassis_Ctrl_Cmd_s *)BoardCommGet(chassis_can_comm);
/****************************************     ControlChassis     *****************************************/
    //底盘动与不动
    ChassisStateSet();
    //旋转模式及速度设定
    ChassisRotateSet();
    // 根据云台和底盘的角度offset将控制量映射到底盘坐标系
    // 根据控制模式进行正运动学解算,计算底盘各个电机的速度
    MecanumCalculate();
    // 设定闭环参考值
    ChassisOutput();
 /*******************************************     SendData     ********************************************/
    //将裁判系统的信息发给巡航，让其进行决策。
    SendJudgeData();
    //根据功率控制板发送的电压来控制速度
    SendPowerData();
    // 根据电机的反馈速度计算真实速度发给巡航
    SendChassisData(); 
    SuperCapSend(cap, (uint8_t*)&power_data);
    BoardCommSend(chassis_can_comm, (void *)&chassis_feedback_data);
}