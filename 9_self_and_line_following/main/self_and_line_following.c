//C Headers
#include <stdio.h>
#include <math.h>
#include <time.h>

//Components
#include "sra_board.h"
#include "tuning_http_server.h"

//FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MODE NORMAL_MODE
#define BLACK_MARGIN 4095
#define WHITE_MARGIN 0
#define bound_LSA_LOW 0
#define bound_LSA_HIGH 1000
#define BLACK_BOUNDARY  700    
// Boundary value to distinguish between black and white readings

//Limiting Parameters
#define MAX_PITCH_CORRECTION 90
#define MAX_PITCH_CUMULATIVE_ERROR 850

#define MAX_PITCH_AREA (850.0f)
#define MAX_PITCH_RATE (850.0f)

#define MAX_PWM 65
#define MIN_PWM 45

#define MAX_PITCH_ERROR -2.5

/*
 * weights given to respective line sensor
 */
const int weights[5] = {-5, -3, 1, 3, 5};

/*
 * Motor value boundts
 */
int optimum_duty_cycle = 45;
int lower_duty_cycle = 30;
int higher_duty_cycle = 60;
float left_duty_cycle = 0, right_duty_cycle = 0;

/*
 * Union containing line sensor readings
 */
line_sensor_array line_sensor_readings;

//Line Following Tuning Parameters
float yaw_kP= 5.6;
float yaw_kI= 0;
float yaw_kD= 1.7;

//Self Balancing Tuning Parameters
float pitch_kP=  10;//5.85;       
float pitch_kI=  0;//95;          
float pitch_kD=  7;

float setpoint = 0	;//-3.5***-2.5
float initial_acce_angle = 0;
float forward_angle = 0;

float forward_offset = 0.25;//0.75***-2.251
float forward_buffer = 18;

//FOR BALANCING
bool balanced = false;
float absolute_pitch_angle = 0;

float pitch_angle=0,roll_angle=0,absolute_pitch_correction=0, pitch_error=0, prevpitch_error=0, pitchDifference=0, pitchCumulativeError=0, pitch_correction=0;

float euler_angle[2], mpu_offset[2] = {0.0f, 0.0f};

//FOR LINE FOLLOWING
float yaw_error=0, yaw_prev_error=0, yaw_difference=0, yaw_cumulative_error=0, yaw_correction=0;
// int weights[4] = {3,1,-1,-3};

float motor_pwm = 0;
float left_pwm  = 0;
float right_pwm = 0;

void lsa_to_bar()
{   
    uint8_t var = 0x00;                     
    bool number[8] = {0,0,0,0,0,0,0,0};
    for(int i = 0; i < 5; i++)
    {
        number[7-i] = (line_sensor_readings.adc_reading[i] < BLACK_BOUNDARY) ? 0 : 1; //If adc value is less than black margin, then set that bit to 0 otherwise 1.
        var = bool_to_uint8(number);  //A helper function to convert bool array to unsigned int.
        ESP_ERROR_CHECK(set_bar_graph(var)); //Setting bar graph led with unsigned int value.
    }
}

static void calc_sensor_values()
{
    line_sensor_readings = read_line_sensor();
    for(int i = 0; i < 4; i++)
    {
        line_sensor_readings.adc_reading[i] = bound(line_sensor_readings.adc_reading[i], WHITE_MARGIN, BLACK_MARGIN);
        line_sensor_readings.adc_reading[i] = map(line_sensor_readings.adc_reading[i], WHITE_MARGIN, BLACK_MARGIN, bound_LSA_LOW, bound_LSA_HIGH);
        line_sensor_readings.adc_reading[i] = 1000 - (line_sensor_readings.adc_reading[i]);
    }
}

void calculate_yaw_error()
{
    int all_black_flag = 1; // assuming initially all black condition
    float weighted_sum = 0, sum = 0; 
    float pos = 0; int k = 0;

    for(int i = 0; i < 5; i++)
    {
        if(line_sensor_readings.adc_reading[i] > BLACK_BOUNDARY)
        {
            all_black_flag = 0;
        }
        if(line_sensor_readings.adc_reading[i] > 700)
        {
            k = 1;
        }
        if(line_sensor_readings.adc_reading[i] < 700)
        {
            k = 0;
        }
        weighted_sum += (float)(weights[i]) * k;
        sum = sum + k;
    }

    if(sum != 0) // sum can never be 0 but just for safety purposes
    {
        pos = (weighted_sum - 1) / sum; // This will give us the position wrt line. if +ve then bot is facing left and if -ve the bot is facing to right.
    }

    if(all_black_flag == 1)  // If all black then we check for previous yaw_error to assign current yaw_error.
    {
        if(yaw_prev_error > 0)
        {
            yaw_error = 2.5;
        }
        else
        {
            yaw_error = -2.5;
        }
    }
    else
    {
        yaw_error = pos;
    }
}

static void calculate_yaw_correction()
{
    yaw_error *= 10; // This line I don't know how it works
    yaw_difference = (yaw_error - yaw_prev_error);
    yaw_cumulative_error += yaw_error;
    
    yaw_cumulative_error = bound(yaw_cumulative_error, -30, 30);

    yaw_correction = read_pid_const().kp_yaw*yaw_error + read_pid_const().ki_yaw*yaw_cumulative_error + read_pid_const().kd_yaw*yaw_difference;
    yaw_prev_error = yaw_error;
}

// Calculate the motor inputs according to angle of the MPU
void calculate_motor_command(const float pitch_error, float *motor_cmd)
{
	
	/** Error values **/
	// Stores pitch error of previous iteration
	static float prev_pitch_error = 0.0f;
	// Stores sum of product of errors with time interval obtained during each iteration
	static float pitch_area = 0.0f;
	// Stores difference between current error and previous iteration error
	float pitch_error_difference = 0.0f;

	/** Correction values **/
	// Variables for storing corrected values
	float pitch_correction = 0.0f, absolute_pitch_correction = 0.0f;
	// Helper variables for calculating integral and derivative correction
	float pitch_rate = 0.0f;

	// Variables storing correction values of different error terms
	float P_term = 0.0f, I_term = 0.0f, D_term = 0.0f;

	// Evaluated delta(error)
	pitch_error_difference = pitch_error - prev_pitch_error;

	// Evaluated area of the graph error vs time (cumulative error)
	pitch_area += (pitch_error);
	// evaluated delta(error)/delta(time) to calculate rate of change in error w.r.t time
	pitch_rate = pitch_error_difference;

	// Calculating p,i and d terms my multuplying corresponding proportional constants
	P_term = read_pid_const().kp_pitch * pitch_error;
	I_term = read_pid_const().ki_pitch * bound(pitch_area, -MAX_PITCH_AREA, MAX_PITCH_AREA);
	D_term = read_pid_const().kd_pitch * bound(pitch_rate, -MAX_PITCH_RATE, MAX_PITCH_RATE);

	pitch_correction = P_term + I_term + D_term;

    // plot_graph(P_term, D_term, I_term, pitch_correction, pitch_error);
	/**
	 * Calculating absolute value of pitch_correction since duty cycle can't be negative. 
	 * Since it is a floating point variable, fabsf was used instead of abs
	*/
	absolute_pitch_correction = fabsf(pitch_correction);

	*motor_cmd = bound(absolute_pitch_correction, 0, MAX_PITCH_CORRECTION);
	prev_pitch_error = pitch_error;
}

//Task for line following while self balancing
void balance_with_line_follow_task(void *arg)
{

    enable_motor_driver(a, NORMAL_MODE);
    enable_line_sensor();
    enable_mpu6050();
    enable_bar_graph();
    float motor_cmd=0.0f;
// #ifdef CONFIG_ENABLE_OLED
//     // Initialising the OLED
//     ESP_ERROR_CHECK(init_oled());
//     vTaskDelay(100);

//     // Clearing the screen
//     lv_obj_clean(lv_scr_act());

// #endif
	/**
	 * euler_angles are the complementary pitch and roll angles obtained from mpu6050
	 * mpu_offsets are the initial accelerometer angles at rest position
	*/

    
	if (enable_mpu6050() == ESP_OK)
	{

        //SELF BALANCING AND LINE FOLLOWING
        while (1) 
        {

            if(read_mpu6050(euler_angle, mpu_offset) == ESP_OK){

                //Calulate PITCH and YAW yaw_error
                // To read PID setpoint from tuning_http_server
                initial_acce_angle = euler_angle[1];
                setpoint = read_pid_const().setpoint;
                pitch_error = setpoint - initial_acce_angle;
                calc_sensor_values();
				calculate_motor_command((pitch_error), &motor_cmd);
                calculate_yaw_error();
                calculate_yaw_correction();

                if(!balanced)
                {

                    // ESP_LOGI("Bot is Not Balanced.... Now, Just Balancing\n");

                    //bound PWM values between max and min
                    motor_pwm = bound((motor_cmd), MIN_PWM, MAX_PWM);

                    ESP_LOGI("debug", "MOTOR_PWM :: %lf", motor_pwm);
                    // Bot tilts upwards
                    if (pitch_error > 1)
                    {
                        // setting motor A0 with definite speed(duty cycle of motor driver PWM) in Backward direction
                        set_motor_speed(MOTOR_A_0, MOTOR_BACKWARD, motor_pwm);
                        // setting motor A1 with definite speed(duty cycle of motor driver PWM) in Backward direction
                        set_motor_speed(MOTOR_A_1, MOTOR_BACKWARD, motor_pwm);
                    }

                    // Bot tilts downwards
                    else if (pitch_error < -1)
                    {
                        // setting motor A0 with definite speed(duty cycle of motor driver PWM) in Forward direction
                        set_motor_speed(MOTOR_A_0, MOTOR_FORWARD, motor_pwm);
                        // setting motor A1 with definite speed(duty cycle of motor driver PWM) in Forward direction
                        set_motor_speed(MOTOR_A_1, MOTOR_FORWARD, motor_pwm);
                    }

                    // Bot remains in desired region for vertical balance
                    else
                    {
                        // stopping motor A0
                        set_motor_speed(MOTOR_A_0, MOTOR_STOP, 0);
                        // stopping motor A1
                        set_motor_speed(MOTOR_A_1, MOTOR_STOP, 0);
                        // Setting the state to balanced
                        balanced = true;
                    }   

                }

                else
                {


                    // ESP_LOGI("Bot is Balanced.... Now, Following the line\n");
                    forward_angle = initial_acce_angle + forward_offset;

                    ESP_LOGI("debug","Yaw correction %lf", yaw_correction);

                    //bound PWM values between max and min values
                    right_pwm = bound((motor_cmd - yaw_correction), MIN_PWM, MAX_PWM);
                    left_pwm = bound((motor_cmd + yaw_correction), MIN_PWM, MAX_PWM);

                    //Extra yaw correction during turns
                    if(yaw_error>10 && right_pwm <= 85)
                    {
                        right_pwm+=15;
                        left_pwm-=15;   
                    }
                    else if(yaw_error<-10 && left_pwm <=85)
                    {
                        left_pwm+=15;
                        right_pwm-=15;
                    }

                    ESP_LOGI("debug", "RIGHT_PWM :: %lf, LEFT_PWM :: %lf", right_pwm, left_pwm);
                    // setting motor A0 with definite speed(duty cycle of motor driver PWM) in Forward direction
                    set_motor_speed(MOTOR_A_0, MOTOR_FORWARD, right_pwm);
                    // setting motor A1 with definite speed(duty cycle of motor driver PWM) in Forward direction
                    set_motor_speed(MOTOR_A_1, MOTOR_FORWARD, left_pwm);    
                    // Change intial_acce_angle back to setpoint if bot exceeds forward_angle buffer
                    ESP_LOGI("debug","pitch correction %lf", pitch_error);
                    if( -pitch_error > forward_buffer || pitch_error > 0)
                    {
                        initial_acce_angle = setpoint;
                        balanced = false;
                    }

                }
            } // End of MPU successfull reading

            // ESP_LOGI("debug","left_duty_cycle:  %f    ::  right_duty_cycle :  %f  :: error :  %f  correction  :  %f  \n",left_duty_cycle, right_duty_cycle, error, correction);
            ESP_LOGI("debug", "KP: %f ::  KI: %f  :: KD: %f", read_pid_const().kp_pitch, read_pid_const().ki_pitch, read_pid_const().kp_pitch);

    // #ifdef CONFIG_ENABLE_OLED
    //         // Diplaying kp, ki, kd values on OLED 
    //         if (read_pid_const().val_changed)
    //         {
    //             display_pid_values(read_pid_const().kp, read_pid_const().ki, read_pid_const().kd);
    //             reset_val_changed_pid_const();
    //         }
    // #endif

            vTaskDelay(10 / portTICK_PERIOD_MS);
        }   //End of While Loop

    }

    vTaskDelete(NULL);

}   //End of Task

void app_main()
{
    xTaskCreate(&balance_with_line_follow_task, "self_bal_and_line_follow_task", 4096, NULL, 1, NULL);
    // vTaskDelay(10 / portTICK_PERIOD_MS);
    start_tuning_http_server();
}