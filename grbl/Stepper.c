/*
  Stepper.c - stepper motor driver: executes motion plans using stepper motors
  Part of Grbl-Advanced

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c)	2017 Patrick F.

  Grbl-Advanced is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl-Advanced is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl-Advanced.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdint.h>
#include <string.h>
#include "Config.h"
#include "Planner.h"
#include "Probe.h"
#include "SpindleControl.h"
#include "System.h"
#include "Settings.h"
#include "util.h"
#include "TIM.h"
#include "Stepper.h"
#include "GPIO.h"
#include "System32.h"


// Some useful constants.
#define DT_SEGMENT						(1.0/(ACCELERATION_TICKS_PER_SECOND*60.0)) // min/segment
#define REQ_MM_INCREMENT_SCALAR 		1.25
#define RAMP_ACCEL						0
#define RAMP_CRUISE						1
#define RAMP_DECEL						2
#define RAMP_DECEL_OVERRIDE				3

#define PREP_FLAG_RECALCULATE			BIT(0)
#define PREP_FLAG_HOLD_PARTIAL_BLOCK	BIT(1)
#define PREP_FLAG_PARKING				BIT(2)
#define PREP_FLAG_DECEL_OVERRIDE		BIT(3)

// Define Adaptive Multi-Axis Step-Smoothing(AMASS) levels and cutoff frequencies. The highest level
// frequency bin starts at 0Hz and ends at its cutoff frequency. The next lower level frequency bin
// starts at the next higher cutoff frequency, and so on. The cutoff frequencies for each level must
// be considered carefully against how much it over-drives the stepper ISR, the accuracy of the 16-bit
// timer, and the CPU overhead. Level 0 (no AMASS, normal operation) frequency bin starts at the
// Level 1 cutoff frequency and up to as fast as the CPU allows (over 30kHz in limited testing).
// NOTE: AMASS cutoff frequency multiplied by ISR overdrive factor must not exceed maximum step frequency.
// NOTE: Current settings are set to overdrive the ISR to no more than 16kHz, balancing CPU overhead
// and timer accuracy.  Do not alter these settings unless you know what you are doing.
#define MAX_AMASS_LEVEL		3
// AMASS_LEVEL0: Normal operation. No AMASS. No upper cutoff frequency. Starts at LEVEL1 cutoff frequency.
#define AMASS_LEVEL1			(uint32_t)(F_TIMER_STEPPER/8000) // Over-drives ISR (x2). Defined as F_CPU/(Cutoff frequency in Hz)
#define AMASS_LEVEL2			(uint32_t)(F_TIMER_STEPPER/4000) // Over-drives ISR (x4)
#define AMASS_LEVEL3			(uint32_t)(F_TIMER_STEPPER/2000) // Over-drives ISR (x8)

#if MAX_AMASS_LEVEL <= 0
  error "AMASS must have 1 or more levels to operate correctly."
#endif

#ifdef MAX_STEP_RATE_HZ
    #define STEP_TIMER_MIN          (uint16_t)(F_TIMER_STEPPER / MAX_STEP_RATE_HZ)
#else
    #define STEP_TIMER_MIN          (uint16_t)((F_TIMER_STEPPER / 60000))
#endif


// Stores the planner block Bresenham algorithm execution data for the segments in the segment
// buffer. Normally, this buffer is partially in-use, but, for the worst case scenario, it will
// never exceed the number of accessible stepper buffer segments (SEGMENT_BUFFER_SIZE-1).
// NOTE: This data is copied from the prepped planner blocks so that the planner blocks may be
// discarded when entirely consumed and completed by the segment buffer. Also, AMASS alters this
// data for its own use.
typedef struct {
	uint32_t steps[N_AXIS];
	uint32_t step_event_count;
	uint8_t direction_bits;
	uint8_t is_pwm_rate_adjusted; // Tracks motions that require constant laser power/rate
} Stepper_Block_t;


// Primary stepper segment ring buffer. Contains small, short line segments for the stepper
// algorithm to execute, which are "checked-out" incrementally from the first block in the
// planner buffer. Once "checked-out", the steps in the segments buffer cannot be modified by
// the planner, where the remaining planner block steps still can.
typedef struct {
	uint16_t n_step;           // Number of step events to be executed for this segment
	uint16_t cycles_per_tick;  // Step distance traveled per ISR tick, aka step rate.
	uint8_t  st_block_index;   // Stepper block data index. Uses this information to execute this segment.
	uint8_t amass_level;    // Indicates AMASS level for the ISR to execute this segment
	uint8_t spindle_pwm;

	uint8_t backlash_motion;
} Stepper_Segment_t;


// Stepper ISR data struct. Contains the running data for the main stepper ISR.
typedef struct {
	// Used by the bresenham line algorithm
	// Counter variables for the bresenham line tracer
	uint32_t counter_x, counter_y, counter_z;

	uint8_t execute_step;     // Flags step execution for each interrupt.
	uint8_t step_pulse_time;  // Step pulse reset time after step rise
	uint8_t step_outbits;         // The next stepping-bits to be output
	uint8_t dir_outbits;
	uint32_t steps[N_AXIS];

	uint16_t step_count;       // Steps remaining in line segment motion
	uint8_t exec_block_index; // Tracks the current st_block index. Change indicates new block.
	Stepper_Block_t *exec_block;   // Pointer to the block data for the segment being executed
	Stepper_Segment_t *exec_segment;  // Pointer to the segment being executed
} Stepper_t;


// Segment preparation data struct. Contains all the necessary information to compute new segments
// based on the current executing planner block.
typedef struct {
	uint8_t st_block_index;  // Index of stepper common data block being prepped
	uint8_t recalculate_flag;

	float dt_remainder;
	float steps_remaining;
	float step_per_mm;
	float req_mm_increment;

#ifdef PARKING_ENABLE
	uint8_t last_st_block_index;
	float last_steps_remaining;
	float last_step_per_mm;
	float last_dt_remainder;
#endif

	uint8_t ramp_type;      // Current segment ramp state
	float mm_complete;      // End of velocity profile from end of current planner block in (mm).
							// NOTE: This value must coincide with a step(no mantissa) when converted.
	float current_speed;    // Current speed at the end of the segment buffer (mm/min)
	float maximum_speed;    // Maximum speed of executing block. Not always nominal speed. (mm/min)
	float exit_speed;       // Exit speed of executing block (mm/min)
	float accelerate_until; // Acceleration ramp end measured from end of block (mm)
	float decelerate_after; // Deceleration ramp start measured from end of block (mm)

	float inv_rate;    // Used by PWM laser mode to speed up segment calculations.
	uint8_t current_spindle_pwm;
} Stepper_PrepData_t;


static Stepper_Block_t st_block_buffer[SEGMENT_BUFFER_SIZE-1];
static Stepper_Segment_t segment_buffer[SEGMENT_BUFFER_SIZE];
static Stepper_t st;

// Step segment ring buffer indices
static volatile uint8_t segment_buffer_tail;
static uint8_t segment_buffer_head;
static uint8_t segment_next_head;

// Step and direction port invert masks.
static uint8_t step_port_invert_mask;
static uint8_t dir_port_invert_mask;

// Pointers for the step segment being prepped from the planner buffer. Accessed only by the
// main program. Pointers may be planning segments or planner blocks ahead of what being executed.
static Planner_Block_t *pl_block;     // Pointer to the planner block being prepped
static Stepper_Block_t *st_prep_block;  // Pointer to the stepper block data being prepped


static Stepper_PrepData_t prep;


/*    BLOCK VELOCITY PROFILE DEFINITION
          __________________________
         /|                        |\     _________________         ^
        / |                        | \   /|               |\        |
       /  |                        |  \ / |               | \       s
      /   |                        |   |  |               |  \      p
     /    |                        |   |  |               |   \     e
    +-----+------------------------+---+--+---------------+----+    e
    |               BLOCK 1            ^      BLOCK 2          |    d
                                       |
                  time ----->      EXAMPLE: Block 2 entry speed is at max junction velocity

  The planner block buffer is planned assuming constant acceleration velocity profiles and are
  continuously joined at block junctions as shown above. However, the planner only actively computes
  the block entry speeds for an optimal velocity plan, but does not compute the block internal
  velocity profiles. These velocity profiles are computed ad-hoc as they are executed by the
  stepper algorithm and consists of only 7 possible types of profiles: cruise-only, cruise-
  deceleration, acceleration-cruise, acceleration-only, deceleration-only, full-trapezoid, and
  triangle(no cruise).

                                        maximum_speed (< nominal_speed) ->  +
                    +--------+ <- maximum_speed (= nominal_speed)          /|\
                   /          \                                           / | \
 current_speed -> +            \                                         /  |  + <- exit_speed
                  |             + <- exit_speed                         /   |  |
                  +-------------+                     current_speed -> +----+--+
                   time -->  ^  ^                                           ^  ^
                             |  |                                           |  |
                decelerate_after(in mm)                             decelerate_after(in mm)
                    ^           ^                                           ^  ^
                    |           |                                           |  |
                accelerate_until(in mm)                             accelerate_until(in mm)

  The step segment buffer computes the executing block velocity profile and tracks the critical
  parameters for the stepper algorithm to accurately trace the profile. These critical parameters
  are shown and defined in the above illustration.
*/


// Initialize and start the stepper motor subsystem
void Stepper_Init(void)
{
	// Configure step and direction interface pins
	GPIO_InitGPIO(GPIO_STEPPER);

	// Init TIM9
	TIM9_Init();
}


// Stepper state initialization. Cycle should only start if the st.cycle_start flag is
// enabled. Startup init and limits call this function but shouldn't start the cycle.
void Stepper_WakeUp(void)
{
	// Enable stepper drivers.
	if(BIT_IS_TRUE(settings.flags, BITFLAG_INVERT_ST_ENABLE)) {
		GPIO_SetBits(GPIO_ENABLE_PORT, GPIO_ENABLE_PIN);
	}
	else {
		GPIO_ResetBits(GPIO_ENABLE_PORT, GPIO_ENABLE_PIN);
	}

    // Give steppers some time to wake up
	Delay_ms(10);

	// Initialize stepper output bits to ensure first ISR call does not step.
	//st.step_outbits = step_port_invert_mask;
	st.step_outbits = 0;

	// Enable Stepper Driver Interrupt
	TIM_Cmd(TIM9, ENABLE);
}


// Stepper shutdown
void Stepper_Disable(uint8_t ovr_disable)
{
	// Disable Stepper Driver Interrupt.
	TIM_Cmd(TIM9, DISABLE);
	Delay_us(1);

	// Reset stepper pins
	Stepper_PortResetISR();

	// Set stepper driver idle state, disabled or enabled, depending on settings and circumstances.
	bool pin_state = false; // Keep enabled.

	if(((settings.stepper_idle_lock_time != 0xFF) || sys_rt_exec_alarm || sys.state == STATE_SLEEP) && sys.state != STATE_HOMING) {
		// Force stepper dwell to lock axes for a defined amount of time to ensure the axes come to a complete
		// stop and not drift from residual inertial forces at the end of the last movement.
		Delay_ms(settings.stepper_idle_lock_time);
		pin_state = true; // Override. Disable steppers.
	}

	if(ovr_disable)
    {
        // Disable
        pin_state = true;
    }

	if(BIT_IS_TRUE(settings.flags, BITFLAG_INVERT_ST_ENABLE)) {
		pin_state = !pin_state;
	} // Apply pin invert.

	if(pin_state) {
		GPIO_SetBits(GPIO_ENABLE_PORT, GPIO_ENABLE_PIN);
	}
	else {
		GPIO_ResetBits(GPIO_ENABLE_PORT, GPIO_ENABLE_PIN);
	}
}


/* "The Stepper Driver Interrupt" - This timer interrupt is the workhorse of Grbl. Grbl employs
   the venerable Bresenham line algorithm to manage and exactly synchronize multi-axis moves.
   Unlike the popular DDA algorithm, the Bresenham algorithm is not susceptible to numerical
   round-off errors and only requires fast integer counters, meaning low computational overhead
   and maximizing the Arduino's capabilities. However, the downside of the Bresenham algorithm
   is, for certain multi-axis motions, the non-dominant axes may suffer from un-smooth step
   pulse trains, or aliasing, which can lead to strange audible noises or shaking. This is
   particularly noticeable or may cause motion issues at low step frequencies (0-5kHz), but
   is usually not a physical problem at higher frequencies, although audible.
     To improve Bresenham multi-axis performance, Grbl uses what we call an Adaptive Multi-Axis
   Step Smoothing (AMASS) algorithm, which does what the name implies. At lower step frequencies,
   AMASS artificially increases the Bresenham resolution without effecting the algorithm's
   innate exactness. AMASS adapts its resolution levels automatically depending on the step
   frequency to be executed, meaning that for even lower step frequencies the step smoothing
   level increases. Algorithmically, AMASS is acheived by a simple bit-shifting of the Bresenham
   step count for each AMASS level. For example, for a Level 1 step smoothing, we bit shift
   the Bresenham step event count, effectively multiplying it by 2, while the axis step counts
   remain the same, and then double the stepper ISR frequency. In effect, we are allowing the
   non-dominant Bresenham axes step in the intermediate ISR tick, while the dominant axis is
   stepping every two ISR ticks, rather than every ISR tick in the traditional sense. At AMASS
   Level 2, we simply bit-shift again, so the non-dominant Bresenham axes can step within any
   of the four ISR ticks, the dominant axis steps every four ISR ticks, and quadruple the
   stepper ISR frequency. And so on. This, in effect, virtually eliminates multi-axis aliasing
   issues with the Bresenham algorithm and does not significantly alter Grbl's performance, but
   in fact, more efficiently utilizes unused CPU cycles overall throughout all configurations.
     AMASS retains the Bresenham algorithm exactness by requiring that it always executes a full
   Bresenham step, regardless of AMASS Level. Meaning that for an AMASS Level 2, all four
   intermediate steps must be completed such that baseline Bresenham (Level 0) count is always
   retained. Similarly, AMASS Level 3 means all eight intermediate steps must be executed.
   Although the AMASS Levels are in reality arbitrary, where the baseline Bresenham counts can
   be multiplied by any integer value, multiplication by powers of two are simply used to ease
   CPU overhead with bitshift integer operations.
     This interrupt is simple and dumb by design. All the computational heavy-lifting, as in
   determining accelerations, is performed elsewhere. This interrupt pops pre-computed segments,
   defined as constant velocity over n number of steps, from the step segment buffer and then
   executes them by pulsing the stepper pins appropriately via the Bresenham algorithm. This
   ISR is supported by The Stepper Port Reset Interrupt which it uses to reset the stepper port
   after each pulse. The bresenham line tracer algorithm controls all stepper outputs
   simultaneously with these two interrupts.

   NOTE: This interrupt must be as efficient as possible and complete before the next ISR tick,
   which for Grbl must be less than 33.3usec (@30kHz ISR rate). Oscilloscope measured time in
   ISR is 5usec typical and 25usec maximum, well below requirement.
   NOTE: This ISR expects at least one step to be executed per segment.
*/
void Stepper_MainISR(void) {
    if(st.step_outbits & (1<<X_STEP_BIT)) {
		if(step_port_invert_mask & (1<<X_STEP_BIT)) {
			// Low pulse
			GPIO_ResetBits(GPIO_STEP_X_PORT, GPIO_STEP_X_PIN);
		}
		else {
			// High pulse
			GPIO_SetBits(GPIO_STEP_X_PORT, GPIO_STEP_X_PIN);
		}
    }
    #ifdef DUAL_X_AXIS
        if(st.step_outbits & (1<<X2_STEP_BIT)) {
            if(step_port_invert_mask & (1<<X2_STEP_BIT)) {
                // Low pulse
                GPIO_ResetBits(GPIO_STEP_X2_PORT, GPIO_STEP_X2_PIN);
            }
            else {
                // High pulse
                GPIO_SetBits(GPIO_STEP_X2_PORT, GPIO_STEP_X2_PIN);
            }
        }
    #endif
    if(st.step_outbits & (1<<Y_STEP_BIT)) {
		if(step_port_invert_mask & (1<<Y_STEP_BIT)) {
			// Low pulse
			GPIO_ResetBits(GPIO_STEP_Y_PORT, GPIO_STEP_Y_PIN);
		}
		else {
			// High pulse
			GPIO_SetBits(GPIO_STEP_Y_PORT, GPIO_STEP_Y_PIN);
		}
    }
    #ifdef DUAL_Y_AXIS
        if(st.step_outbits & (1<<Y2_STEP_BIT)) {
            if(step_port_invert_mask & (1<<Y2_STEP_BIT)) {
                // Low pulse
                GPIO_ResetBits(GPIO_STEP_Y2_PORT, GPIO_STEP_Y2_PIN);
            }
            else {
                // High pulse
                GPIO_SetBits(GPIO_STEP_Y2_PORT, GPIO_STEP_Y2_PIN);
            }
        }
    #endif
    if(st.step_outbits & (1<<Z_STEP_BIT)) {
		if(step_port_invert_mask & (1<<Z_STEP_BIT)) {
			// Low pulse
			GPIO_ResetBits(GPIO_STEP_Z_PORT, GPIO_STEP_Z_PIN);
		}
		else {
			// High pulse
			GPIO_SetBits(GPIO_STEP_Z_PORT, GPIO_STEP_Z_PIN);
		}
    }

	// If there is no step segment, attempt to pop one from the stepper buffer
	if(st.exec_segment == 0) {
		// Anything in the buffer? If so, load and initialize next step segment.
		if(segment_buffer_head != segment_buffer_tail) {
			// Initialize new step segment and load number of steps to execute
			st.exec_segment = &segment_buffer[segment_buffer_tail];

			// Initialize step segment timing per step and load number of steps to execute.
			// Limit ISR to 50 KHz
			if(st.exec_segment->cycles_per_tick < STEP_TIMER_MIN) {
				st.exec_segment->cycles_per_tick = STEP_TIMER_MIN;
			}

			TIM9->ARR = st.exec_segment->cycles_per_tick;
			TIM9->CCR1 = (uint16_t)(st.exec_segment->cycles_per_tick * 0.75);
			st.step_count = st.exec_segment->n_step; // NOTE: Can sometimes be zero when moving slow.

			// If the new segment starts a new planner block, initialize stepper variables and counters.
			// NOTE: When the segment data index changes, this indicates a new planner block.
			if(st.exec_block_index != st.exec_segment->st_block_index) {
				st.exec_block_index = st.exec_segment->st_block_index;
				st.exec_block = &st_block_buffer[st.exec_block_index];

				// Initialize Bresenham line and distance counters
				st.counter_x = st.counter_y = st.counter_z = (st.exec_block->step_event_count >> 1);
			}

			st.dir_outbits = st.exec_block->direction_bits ^ dir_port_invert_mask;

			// Set the direction pins directly here to make sure that the signal is valid when stepping the steppers
			// Some driver e.g. require a setup time of a few us.
			if(st.dir_outbits & (1<<X_DIRECTION_BIT)) {
				GPIO_SetBits(GPIO_DIR_X_PORT, GPIO_DIR_X_PIN);
			}
			else {
				GPIO_ResetBits(GPIO_DIR_X_PORT, GPIO_DIR_X_PIN);
			}

			#ifdef DUAL_X_AXIS
                if(st.dir_outbits & (1<<X2_DIRECTION_BIT)) {
                    #ifndef INVERT_DUAL_X_AXIS
                        GPIO_SetBits(GPIO_DIR_X2_PORT, GPIO_DIR_X2_PIN);
                    #else
                        GPIO_ResetBits(GPIO_DIR_X2_PORT, GPIO_DIR_X2_PIN);
                    #endif
                }
                else {
                    #ifndef INVERT_DUAL_X_AXIS
                        GPIO_ResetBits(GPIO_DIR_X2_PORT, GPIO_DIR_X2_PIN);
                    #else
                        GPIO_SetBits(GPIO_DIR_X2_PORT, GPIO_DIR_X2_PIN);
                    #endif
                }
			#endif

			if(st.dir_outbits & (1<<Y_DIRECTION_BIT)) {
				GPIO_SetBits(GPIO_DIR_Y_PORT, GPIO_DIR_Y_PIN);
			}
			else {
				GPIO_ResetBits(GPIO_DIR_Y_PORT, GPIO_DIR_Y_PIN);
			}

			#ifdef DUAL_Y_AXIS
                if(st.dir_outbits & (1<<Y2_DIRECTION_BIT)) {
                    #ifndef INVERT_DUAL_Y_AXIS
                        GPIO_SetBits(GPIO_DIR_Y2_PORT, GPIO_DIR_Y2_PIN);
                    #else
                        GPIO_ResetBits(GPIO_DIR_Y2_PORT, GPIO_DIR_Y2_PIN);
                    #endif
                }
                else {
                    #ifndef INVERT_DUAL_Y_AXIS
                        GPIO_ResetBits(GPIO_DIR_Y2_PORT, GPIO_DIR_Y2_PIN);
                    #else
                        GPIO_SetBits(GPIO_DIR_Y2_PORT, GPIO_DIR_Y2_PIN);
                    #endif
                }
            #endif

			if(st.dir_outbits & (1<<Z_DIRECTION_BIT)) {
				GPIO_SetBits(GPIO_DIR_Z_PORT, GPIO_DIR_Z_PIN);
			}
			else {
				GPIO_ResetBits(GPIO_DIR_Z_PORT, GPIO_DIR_Z_PIN);
			}

			// With AMASS enabled, adjust Bresenham axis increment counters according to AMASS level.
			st.steps[X_AXIS] = st.exec_block->steps[X_AXIS] >> st.exec_segment->amass_level;
			st.steps[Y_AXIS] = st.exec_block->steps[Y_AXIS] >> st.exec_segment->amass_level;
			st.steps[Z_AXIS] = st.exec_block->steps[Z_AXIS] >> st.exec_segment->amass_level;

			// Set real-time spindle output as segment is loaded, just prior to the first step.
			Spindle_SetSpeed(st.exec_segment->spindle_pwm);

		}
		else {
			// Segment buffer empty. Shutdown.
			Stepper_Disable(0);

			// Ensure pwm is set properly upon completion of rate-controlled motion.
			if(st.exec_block->is_pwm_rate_adjusted) {
				Spindle_SetSpeed(SPINDLE_PWM_OFF_VALUE);
			}
			System_SetExecStateFlag(EXEC_CYCLE_STOP); // Flag main program for cycle end

			return; // Nothing to do but exit.
		}
	}


	// Check probing state.
	if(sys_probe_state == PROBE_ACTIVE) {
		Probe_StateMonitor();
	}

	// Reset step out bits.
	st.step_outbits = 0;

	// Execute step displacement profile by Bresenham line algorithm
	st.counter_x += st.steps[X_AXIS];

	if(st.counter_x > st.exec_block->step_event_count) {
        #ifndef DUAL_X_AXIS
            st.step_outbits |= (1<<X_STEP_BIT);
        #else
            st.step_outbits |= (1<<X_STEP_BIT) | (1<<X2_STEP_BIT);
        #endif
		st.counter_x -= st.exec_block->step_event_count;

        if(st.exec_segment->backlash_motion == 0) {
            if(st.exec_block->direction_bits & (1<<X_DIRECTION_BIT)) {
                sys_position[X_AXIS]--;
            }
            else {
                sys_position[X_AXIS]++;
            }
        }
	}

	st.counter_y += st.steps[Y_AXIS];

	if(st.counter_y > st.exec_block->step_event_count) {
		#ifndef DUAL_Y_AXIS
            st.step_outbits |= (1<<Y_STEP_BIT);
        #else
            st.step_outbits |= (1<<Y_STEP_BIT) | (1<<Y2_STEP_BIT);
        #endif

		st.counter_y -= st.exec_block->step_event_count;

        if(st.exec_segment->backlash_motion == 0)
        {
            if(st.exec_block->direction_bits & (1<<Y_DIRECTION_BIT)) {
                sys_position[Y_AXIS]--;
            }
            else {
                sys_position[Y_AXIS]++;
            }
        }
	}

	st.counter_z += st.steps[Z_AXIS];

	if(st.counter_z > st.exec_block->step_event_count) {
		st.step_outbits |= (1<<Z_STEP_BIT);
		st.counter_z -= st.exec_block->step_event_count;

        if(st.exec_segment->backlash_motion == 0)
        {
            if(st.exec_block->direction_bits & (1<<Z_DIRECTION_BIT)) {
                sys_position[Z_AXIS]--;
            }
            else {
                sys_position[Z_AXIS]++;
            }
        }
	}

	// During a homing cycle, lock out and prevent desired axes from moving.
	if(sys.state == STATE_HOMING) {
		st.step_outbits &= sys.homing_axis_lock;
	}

	st.step_count--; // Decrement step events count
	if(st.step_count == 0) {
		// Segment is complete. Discard current segment and advance segment indexing.
		st.exec_segment = 0;

		if(++segment_buffer_tail == SEGMENT_BUFFER_SIZE) {
			segment_buffer_tail = 0;
		}
	}
}


/* The Stepper Port Reset Interrupt: Timer9 OVF interrupt handles the falling edge of the step
   pulse.
   NOTE: Interrupt collisions between the serial and stepper interrupts can cause delays by
   a few microseconds, if they execute right before one another. Not a big deal, but can
   cause issues at high step rates if another high frequency asynchronous interrupt is
   added to Grbl.
*/
void Stepper_PortResetISR(void)
{
	// Reset stepping pins (leave the direction pins)

	// X
	if(step_port_invert_mask & (1<<X_STEP_BIT)) {
		GPIO_SetBits(GPIO_STEP_X_PORT, GPIO_STEP_X_PIN);
	}
	else {
		GPIO_ResetBits(GPIO_STEP_X_PORT, GPIO_STEP_X_PIN);
	}

	//X2
    #ifdef DUAL_X_AXIS
        if(step_port_invert_mask & (1<<X2_STEP_BIT)) {
            GPIO_SetBits(GPIO_STEP_X2_PORT, GPIO_STEP_X2_PIN);
        }
        else {
            GPIO_ResetBits(GPIO_STEP_X2_PORT, GPIO_STEP_X2_PIN);
        }
    #endif

	// Y
	if(step_port_invert_mask & (1<<Y_STEP_BIT)) {
		GPIO_SetBits(GPIO_STEP_Y_PORT, GPIO_STEP_Y_PIN);
	}
	else {
		GPIO_ResetBits(GPIO_STEP_Y_PORT, GPIO_STEP_Y_PIN);
	}

    // Y2
    #ifdef DUAL_Y_AXIS
        if(step_port_invert_mask & (1<<Y2_STEP_BIT)) {
            GPIO_SetBits(GPIO_STEP_Y2_PORT, GPIO_STEP_Y2_PIN);
        }
        else {
            GPIO_ResetBits(GPIO_STEP_Y2_PORT, GPIO_STEP_Y2_PIN);
        }
    #endif

	// Z
	if(step_port_invert_mask & (1<<Z_STEP_BIT)) {
		GPIO_SetBits(GPIO_STEP_Z_PORT, GPIO_STEP_Z_PIN);
	}
	else {
		GPIO_ResetBits(GPIO_STEP_Z_PORT, GPIO_STEP_Z_PIN);
	}
}


// Generates the step and direction port invert masks used in the Stepper Interrupt Driver.
void Stepper_GenerateStepDirInvertMasks(void)
{
	uint8_t idx;

	step_port_invert_mask = 0;
	dir_port_invert_mask = 0;

	for(idx = 0; idx < N_AXIS; idx++) {
		if(BIT_IS_TRUE(settings.step_invert_mask, BIT(idx))) {
			step_port_invert_mask |= Settings_GetStepPinMask(idx);
		}

		if(BIT_IS_TRUE(settings.dir_invert_mask, BIT(idx))) {
			dir_port_invert_mask |= Settings_GetDirectionPinMask(idx);
		}
	}
}


// Reset and clear stepper subsystem variables
void Stepper_Reset(void)
{
	// Initialize stepper driver idle state.
	Stepper_Disable(0);

	// Initialize stepper algorithm variables.
	memset(&prep, 0, sizeof(Stepper_PrepData_t));
	memset(&st, 0, sizeof(Stepper_t));

	st.exec_segment = 0;
	pl_block = 0;  // Planner block pointer used by segment buffer
	segment_buffer_tail = 0;
	segment_buffer_head = 0; // empty = tail
	segment_next_head = 1;

	Stepper_GenerateStepDirInvertMasks();
	st.dir_outbits = dir_port_invert_mask; // Initialize direction bits to default.

	// Initialize step and direction port pins.
	// TODO: Stepper invert mask
	// Reset Step Pins
	GPIO_ResetBits(GPIO_STEP_X_PORT, GPIO_STEP_X_PIN);
	#ifdef DUAL_X_AXIS
        GPIO_ResetBits(GPIO_STEP_X2_PORT, GPIO_STEP_X2_PIN);
    #endif
	GPIO_ResetBits(GPIO_STEP_Y_PORT, GPIO_STEP_Y_PIN);
	#ifdef DUAL_Y_AXIS
        GPIO_ResetBits(GPIO_STEP_Y2_PORT, GPIO_STEP_Y2_PIN);
    #endif
	GPIO_ResetBits(GPIO_STEP_Z_PORT, GPIO_STEP_Z_PIN);

	// Reset Direction Pins
	GPIO_ResetBits(GPIO_DIR_X_PORT, GPIO_DIR_X_PIN);
	#ifdef DUAL_X_AXIS
        GPIO_ResetBits(GPIO_DIR_X2_PORT, GPIO_DIR_X2_PIN);
    #endif
	GPIO_ResetBits(GPIO_DIR_Y_PORT, GPIO_DIR_Y_PIN);
	#ifdef DUAL_Y_AXIS
        GPIO_ResetBits(GPIO_DIR_Y2_PORT, GPIO_DIR_Y2_PIN);
    #endif
	GPIO_ResetBits(GPIO_DIR_Z_PORT, GPIO_DIR_Z_PIN);
}


// Called by planner_recalculate() when the executing block is updated by the new plan.
void Stepper_UpdatePlannerBlockParams(void)
{
	if(pl_block != 0) { // Ignore if at start of a new block.
		prep.recalculate_flag |= PREP_FLAG_RECALCULATE;
		pl_block->entry_speed_sqr = prep.current_speed*prep.current_speed; // Update entry speed.
		pl_block = 0; // Flag st_prep_segment() to load and check active velocity profile.
	}
}


// Increments the step segment buffer block data ring buffer.
static uint8_t Stepper_NextBlockIndex(uint8_t block_index)
{
	block_index++;

	if(block_index == (SEGMENT_BUFFER_SIZE-1)) {
		return(0);
	}

	return block_index;
}


#ifdef PARKING_ENABLE
// Changes the run state of the step segment buffer to execute the special parking motion.
void Stepper_ParkingSetupBuffer()
{
    // Store step execution data of partially completed block, if necessary.
    if(prep.recalculate_flag & PREP_FLAG_HOLD_PARTIAL_BLOCK) {
		prep.last_st_block_index = prep.st_block_index;
		prep.last_steps_remaining = prep.steps_remaining;
		prep.last_dt_remainder = prep.dt_remainder;
		prep.last_step_per_mm = prep.step_per_mm;
    }
    // Set flags to execute a parking motion
    prep.recalculate_flag |= PREP_FLAG_PARKING;
    prep.recalculate_flag &= ~(PREP_FLAG_RECALCULATE);
    pl_block = 0; // Always reset parking motion to reload new block.
}


// Restores the step segment buffer to the normal run state after a parking motion.
void Stepper_ParkingRestoreBuffer()
{
    // Restore step execution data and flags of partially completed block, if necessary.
    if(prep.recalculate_flag & PREP_FLAG_HOLD_PARTIAL_BLOCK) {
		st_prep_block = &st_block_buffer[prep.last_st_block_index];
		prep.st_block_index = prep.last_st_block_index;
		prep.steps_remaining = prep.last_steps_remaining;
		prep.dt_remainder = prep.last_dt_remainder;
		prep.step_per_mm = prep.last_step_per_mm;
		prep.recalculate_flag = (PREP_FLAG_HOLD_PARTIAL_BLOCK | PREP_FLAG_RECALCULATE);
		prep.req_mm_increment = REQ_MM_INCREMENT_SCALAR/prep.step_per_mm; // Recompute this value.
    }
    else {
		prep.recalculate_flag = false;
    }

    pl_block = NULL; // Set to reload next block.
}
#endif


/* Prepares step segment buffer. Continuously called from main program.

   The segment buffer is an intermediary buffer interface between the execution of steps
   by the stepper algorithm and the velocity profiles generated by the planner. The stepper
   algorithm only executes steps within the segment buffer and is filled by the main program
   when steps are "checked-out" from the first block in the planner buffer. This keeps the
   step execution and planning optimization processes atomic and protected from each other.
   The number of steps "checked-out" from the planner buffer and the number of segments in
   the segment buffer is sized and computed such that no operation in the main program takes
   longer than the time it takes the stepper algorithm to empty it before refilling it.
   Currently, the segment buffer conservatively holds roughly up to 40-50 msec of steps.
   NOTE: Computation units are in steps, millimeters, and minutes.
*/
void Stepper_PrepareBuffer(void)
{
	// Block step prep buffer, while in a suspend state and there is no suspend motion to execute.
	if(BIT_IS_TRUE(sys.step_control,STEP_CONTROL_END_MOTION)) {
		return;
	}

	while(segment_buffer_tail != segment_next_head) { // Check if we need to fill the buffer.
		// Determine if we need to load a new planner block or if the block needs to be recomputed.
		if(pl_block == 0) {
			// Query planner for a queued block
			if(sys.step_control & STEP_CONTROL_EXECUTE_SYS_MOTION) {
				pl_block = Planner_GetSystemMotionBlock();
			}
			else {
				pl_block = Planner_GetCurrentBlock();
			}

			if(pl_block == 0) {
				// No planner blocks. Exit.
				return;
			}

			// Check if we need to only recompute the velocity profile or load a new block.
			if(prep.recalculate_flag & PREP_FLAG_RECALCULATE) {
#ifdef PARKING_ENABLE
				if(prep.recalculate_flag & PREP_FLAG_PARKING) {
					prep.recalculate_flag &= ~(PREP_FLAG_RECALCULATE);
				}
				else {
					prep.recalculate_flag = false;
				}
#else
				prep.recalculate_flag = false;
#endif
			}
			else {
				// Load the Bresenham stepping data for the block.
				prep.st_block_index = Stepper_NextBlockIndex(prep.st_block_index);

				// Prepare and copy Bresenham algorithm segment data from the new planner block, so that
				// when the segment buffer completes the planner block, it may be discarded when the
				// segment buffer finishes the prepped block, but the stepper ISR is still executing it.
				st_prep_block = &st_block_buffer[prep.st_block_index];
				st_prep_block->direction_bits = pl_block->direction_bits;

				uint8_t idx;
				// With AMASS enabled, simply bit-shift multiply all Bresenham data by the max AMASS
				// level, such that we never divide beyond the original data anywhere in the algorithm.
				// If the original data is divided, we can lose a step from integer roundoff.
				for(idx = 0; idx < N_AXIS; idx++) {
					st_prep_block->steps[idx] = pl_block->steps[idx] << MAX_AMASS_LEVEL;
				}

				st_prep_block->step_event_count = pl_block->step_event_count << MAX_AMASS_LEVEL;

				// Initialize segment buffer data for generating the segments.
				prep.steps_remaining = (float)pl_block->step_event_count;
				prep.step_per_mm = prep.steps_remaining/pl_block->millimeters;
				prep.req_mm_increment = REQ_MM_INCREMENT_SCALAR/prep.step_per_mm;
				prep.dt_remainder = 0.0; // Reset for new segment block

				if((sys.step_control & STEP_CONTROL_EXECUTE_HOLD) || (prep.recalculate_flag & PREP_FLAG_DECEL_OVERRIDE)) {
					// New block loaded mid-hold. Override planner block entry speed to enforce deceleration.
					prep.current_speed = prep.exit_speed;
					pl_block->entry_speed_sqr = prep.exit_speed*prep.exit_speed;
					prep.recalculate_flag &= ~(PREP_FLAG_DECEL_OVERRIDE);
				}
				else {
					prep.current_speed = sqrt(pl_block->entry_speed_sqr);
				}

				// Setup laser mode variables. PWM rate adjusted motions will always complete a motion with the
				// spindle off.
				st_prep_block->is_pwm_rate_adjusted = false;

				if(settings.flags & BITFLAG_LASER_MODE) {
					if(pl_block->condition & PL_COND_FLAG_SPINDLE_CCW) {
						// Pre-compute inverse programmed rate to speed up PWM updating per step segment.
						prep.inv_rate = 1.0/pl_block->programmed_rate;
						st_prep_block->is_pwm_rate_adjusted = true;
					}
				}
			}

			/* ---------------------------------------------------------------------------------
			Compute the velocity profile of a new planner block based on its entry and exit
			speeds, or recompute the profile of a partially-completed planner block if the
			planner has updated it. For a commanded forced-deceleration, such as from a feed
			hold, override the planner velocities and decelerate to the target exit speed.
			*/
			prep.mm_complete = 0.0; // Default velocity profile complete at 0.0mm from end of block.
			float inv_2_accel = 0.5/pl_block->acceleration;

			if(sys.step_control & STEP_CONTROL_EXECUTE_HOLD) { // [Forced Deceleration to Zero Velocity]
				// Compute velocity profile parameters for a feed hold in-progress. This profile overrides
				// the planner block profile, enforcing a deceleration to zero speed.
				prep.ramp_type = RAMP_DECEL;
				// Compute decelerate distance relative to end of block.
				float decel_dist = pl_block->millimeters - inv_2_accel*pl_block->entry_speed_sqr;

				if(decel_dist < 0.0) {
					// Deceleration through entire planner block. End of feed hold is not in this block.
					prep.exit_speed = sqrt(pl_block->entry_speed_sqr-2*pl_block->acceleration*pl_block->millimeters);
				}
				else {
					prep.mm_complete = decel_dist; // End of feed hold.
					prep.exit_speed = 0.0;
				}
			}
			else { // [Normal Operation]
				// Compute or recompute velocity profile parameters of the prepped planner block.
				prep.ramp_type = RAMP_ACCEL; // Initialize as acceleration ramp.
				prep.accelerate_until = pl_block->millimeters;

				float exit_speed_sqr;
				float nominal_speed;

				if(sys.step_control & STEP_CONTROL_EXECUTE_SYS_MOTION) {
					prep.exit_speed = exit_speed_sqr = 0.0; // Enforce stop at end of system motion.
				}
				else {
					exit_speed_sqr = Planner_GetExecBlockExitSpeedSqr();
					prep.exit_speed = sqrt(exit_speed_sqr);
				}

				nominal_speed = Planner_ComputeProfileNominalSpeed(pl_block);

				float nominal_speed_sqr = nominal_speed*nominal_speed;
				float intersect_distance = 0.5*(pl_block->millimeters+inv_2_accel*(pl_block->entry_speed_sqr-exit_speed_sqr));

				if(pl_block->entry_speed_sqr > nominal_speed_sqr) { // Only occurs during override reductions.
					prep.accelerate_until = pl_block->millimeters - inv_2_accel*(pl_block->entry_speed_sqr-nominal_speed_sqr);
					if(prep.accelerate_until <= 0.0) { // Deceleration-only.
						prep.ramp_type = RAMP_DECEL;
						// prep.decelerate_after = pl_block->millimeters;
						// prep.maximum_speed = prep.current_speed;

						// Compute override block exit speed since it doesn't match the planner exit speed.
						prep.exit_speed = sqrt(pl_block->entry_speed_sqr - 2*pl_block->acceleration*pl_block->millimeters);
						prep.recalculate_flag |= PREP_FLAG_DECEL_OVERRIDE; // Flag to load next block as deceleration override.

						// TODO: Determine correct handling of parameters in deceleration-only.
						// Can be tricky since entry speed will be current speed, as in feed holds.
						// Also, look into near-zero speed handling issues with this.
					}
					else {
						// Decelerate to cruise or cruise-decelerate types. Guaranteed to intersect updated plan.
						prep.decelerate_after = inv_2_accel*(nominal_speed_sqr-exit_speed_sqr);
						prep.maximum_speed = nominal_speed;
						prep.ramp_type = RAMP_DECEL_OVERRIDE;
					}
				} else if(intersect_distance > 0.0) {
					if (intersect_distance < pl_block->millimeters) { // Either trapezoid or triangle types
						// NOTE: For acceleration-cruise and cruise-only types, following calculation will be 0.0.
						prep.decelerate_after = inv_2_accel*(nominal_speed_sqr-exit_speed_sqr);
						if(prep.decelerate_after < intersect_distance) { // Trapezoid type
							prep.maximum_speed = nominal_speed;

							if(pl_block->entry_speed_sqr == nominal_speed_sqr) {
								// Cruise-deceleration or cruise-only type.
								prep.ramp_type = RAMP_CRUISE;
							}
							else {
								// Full-trapezoid or acceleration-cruise types
								prep.accelerate_until -= inv_2_accel*(nominal_speed_sqr-pl_block->entry_speed_sqr);
							}
						}
						else { // Triangle type
							prep.accelerate_until = intersect_distance;
							prep.decelerate_after = intersect_distance;
							prep.maximum_speed = sqrt(2.0*pl_block->acceleration*intersect_distance+exit_speed_sqr);
						}
					}
					else { // Deceleration-only type
						prep.ramp_type = RAMP_DECEL;
						// prep.decelerate_after = pl_block->millimeters;
						// prep.maximum_speed = prep.current_speed;
					}
				} else { // Acceleration-only type
					prep.accelerate_until = 0.0;
					// prep.decelerate_after = 0.0;
					prep.maximum_speed = prep.exit_speed;
				}
			}

			BIT_TRUE(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM); // Force update whenever updating block.
		}

		// Initialize new segment
		Stepper_Segment_t *prep_segment = &segment_buffer[segment_buffer_head];

		// Set new segment to point to the current segment data block.
		prep_segment->st_block_index = prep.st_block_index;

		prep_segment->backlash_motion = pl_block->backlash_motion;

		/*------------------------------------------------------------------------------------
		Compute the average velocity of this new segment by determining the total distance
		traveled over the segment time DT_SEGMENT. The following code first attempts to create
		a full segment based on the current ramp conditions. If the segment time is incomplete
		when terminating at a ramp state change, the code will continue to loop through the
		progressing ramp states to fill the remaining segment execution time. However, if
		an incomplete segment terminates at the end of the velocity profile, the segment is
		considered completed despite having a truncated execution time less than DT_SEGMENT.
		The velocity profile is always assumed to progress through the ramp sequence:
		acceleration ramp, cruising state, and deceleration ramp. Each ramp's travel distance
		may range from zero to the length of the block. Velocity profiles can end either at
		the end of planner block (typical) or mid-block at the end of a forced deceleration,
		such as from a feed hold.
		*/
		float dt_max = DT_SEGMENT; // Maximum segment time
		float dt = 0.0; // Initialize segment time
		float time_var = dt_max; // Time worker variable
		float mm_var; // mm-Distance worker variable
		float speed_var; // Speed worker variable
		float mm_remaining = pl_block->millimeters; // New segment distance from end of block.
		float minimum_mm = mm_remaining-prep.req_mm_increment; // Guarantee at least one step.

		if(minimum_mm < 0.0) {
			minimum_mm = 0.0;
		}

		do {
			switch(prep.ramp_type)
			{
			case RAMP_DECEL_OVERRIDE:
				speed_var = pl_block->acceleration*time_var;
				mm_var = time_var*(prep.current_speed - 0.5*speed_var);
				mm_remaining -= mm_var;

				if((mm_remaining < prep.accelerate_until) || (mm_var <= 0)) {
					// Cruise or cruise-deceleration types only for deceleration override.
					mm_remaining = prep.accelerate_until; // NOTE: 0.0 at EOB
					time_var = 2.0*(pl_block->millimeters-mm_remaining)/(prep.current_speed+prep.maximum_speed);
					prep.ramp_type = RAMP_CRUISE;
					prep.current_speed = prep.maximum_speed;
				}
				else { // Mid-deceleration override ramp.
					prep.current_speed -= speed_var;
				}
				break;

			case RAMP_ACCEL:
				// NOTE: Acceleration ramp only computes during first do-while loop.
				speed_var = pl_block->acceleration*time_var;
				mm_remaining -= time_var*(prep.current_speed + 0.5*speed_var);

				if(mm_remaining < prep.accelerate_until) { // End of acceleration ramp.
					// Acceleration-cruise, acceleration-deceleration ramp junction, or end of block.
					mm_remaining = prep.accelerate_until; // NOTE: 0.0 at EOB
					time_var = 2.0*(pl_block->millimeters-mm_remaining)/(prep.current_speed+prep.maximum_speed);

					if(mm_remaining == prep.decelerate_after) {
						prep.ramp_type = RAMP_DECEL;
					}
					else { prep.ramp_type = RAMP_CRUISE; }
					prep.current_speed = prep.maximum_speed;
				}
				else { // Acceleration only.
					prep.current_speed += speed_var;
				}
				break;

			case RAMP_CRUISE:
				// NOTE: mm_var used to retain the last mm_remaining for incomplete segment time_var calculations.
				// NOTE: If maximum_speed*time_var value is too low, round-off can cause mm_var to not change. To
				//   prevent this, simply enforce a minimum speed threshold in the planner.
				mm_var = mm_remaining - prep.maximum_speed*time_var;

				if(mm_var < prep.decelerate_after) { // End of cruise.
					// Cruise-deceleration junction or end of block.
					time_var = (mm_remaining - prep.decelerate_after)/prep.maximum_speed;
					mm_remaining = prep.decelerate_after; // NOTE: 0.0 at EOB
					prep.ramp_type = RAMP_DECEL;
				}
				else { // Cruising only.
					mm_remaining = mm_var;
				}
				break;

			default: // case RAMP_DECEL:
				// NOTE: mm_var used as a misc worker variable to prevent errors when near zero speed.
				speed_var = pl_block->acceleration*time_var; // Used as delta speed (mm/min)

				if(prep.current_speed > speed_var) { // Check if at or below zero speed.
					// Compute distance from end of segment to end of block.
					mm_var = mm_remaining - time_var*(prep.current_speed - 0.5*speed_var); // (mm)

					if(mm_var > prep.mm_complete) { // Typical case. In deceleration ramp.
						mm_remaining = mm_var;
						prep.current_speed -= speed_var;
						break; // Segment complete. Exit switch-case statement. Continue do-while loop.
					}
				}
				// Otherwise, at end of block or end of forced-deceleration.
				time_var = 2.0*(mm_remaining-prep.mm_complete)/(prep.current_speed+prep.exit_speed);
				mm_remaining = prep.mm_complete;
				prep.current_speed = prep.exit_speed;
			}

			dt += time_var; // Add computed ramp time to total segment time.

			if(dt < dt_max) {
				time_var = dt_max - dt;
			} // **Incomplete** At ramp junction.
			else {
				if(mm_remaining > minimum_mm) { // Check for very slow segments with zero steps.
					// Increase segment time to ensure at least one step in segment. Override and loop
					// through distance calculations until minimum_mm or mm_complete.
					dt_max += DT_SEGMENT;
					time_var = dt_max - dt;
				}
				else {
					break; // **Complete** Exit loop. Segment execution time maxed.
				}
			}
		} while(mm_remaining > prep.mm_complete); // **Complete** Exit loop. Profile complete.

		/* -----------------------------------------------------------------------------------
		Compute spindle speed PWM output for step segment
		*/

		if(st_prep_block->is_pwm_rate_adjusted || (sys.step_control & STEP_CONTROL_UPDATE_SPINDLE_PWM)) {
			if(pl_block->condition & (PL_COND_FLAG_SPINDLE_CW | PL_COND_FLAG_SPINDLE_CCW)) {
				float rpm = pl_block->spindle_speed;

				// NOTE: Feed and rapid overrides are independent of PWM value and do not alter laser power/rate.
				if(st_prep_block->is_pwm_rate_adjusted) {
					rpm *= (prep.current_speed * prep.inv_rate);
				}

				// If current_speed is zero, then may need to be rpm_min*(100/MAX_SPINDLE_SPEED_OVERRIDE)
				// but this would be instantaneous only and during a motion. May not matter at all.
				prep.current_spindle_pwm = Spindle_ComputePwmValue(rpm);
			}
			else {
				sys.spindle_speed = 0.0;
				prep.current_spindle_pwm = SPINDLE_PWM_OFF_VALUE;
			}

			BIT_FALSE(sys.step_control, STEP_CONTROL_UPDATE_SPINDLE_PWM);
		}

		prep_segment->spindle_pwm = prep.current_spindle_pwm; // Reload segment PWM value


		/* -----------------------------------------------------------------------------------
		Compute segment step rate, steps to execute, and apply necessary rate corrections.
		NOTE: Steps are computed by direct scalar conversion of the millimeter distance
		remaining in the block, rather than incrementally tallying the steps executed per
		segment. This helps in removing floating point round-off issues of several additions.
		However, since floats have only 7.2 significant digits, long moves with extremely
		high step counts can exceed the precision of floats, which can lead to lost steps.
		Fortunately, this scenario is highly unlikely and unrealistic in CNC machines
		supported by Grbl (i.e. exceeding 10 meters axis travel at 200 step/mm).
		*/
		float step_dist_remaining = prep.step_per_mm*mm_remaining; // Convert mm_remaining to steps
		float n_steps_remaining = ceil(step_dist_remaining); // Round-up current steps remaining
		float last_n_steps_remaining = ceil(prep.steps_remaining); // Round-up last steps remaining
		prep_segment->n_step = last_n_steps_remaining-n_steps_remaining; // Compute number of steps to execute.

		// Bail if we are at the end of a feed hold and don't have a step to execute.
		if(prep_segment->n_step == 0) {
			if(sys.step_control & STEP_CONTROL_EXECUTE_HOLD) {
				// Less than one step to decelerate to zero speed, but already very close. AMASS
				// requires full steps to execute. So, just bail.
				BIT_TRUE(sys.step_control, STEP_CONTROL_END_MOTION);
#ifdef PARKING_ENABLE
					if(!(prep.recalculate_flag & PREP_FLAG_PARKING)) {
						prep.recalculate_flag |= PREP_FLAG_HOLD_PARTIAL_BLOCK;
					}
#endif
				return; // Segment not generated, but current step data still retained.
			}
		}

		// Compute segment step rate. Since steps are integers and mm distances traveled are not,
		// the end of every segment can have a partial step of varying magnitudes that are not
		// executed, because the stepper ISR requires whole steps due to the AMASS algorithm. To
		// compensate, we track the time to execute the previous segment's partial step and simply
		// apply it with the partial step distance to the current segment, so that it minutely
		// adjusts the whole segment rate to keep step output exact. These rate adjustments are
		// typically very small and do not adversely effect performance, but ensures that Grbl
		// outputs the exact acceleration and velocity profiles as computed by the planner.
		dt += prep.dt_remainder; // Apply previous segment partial step execute time

		float inv_rate = dt/(last_n_steps_remaining - step_dist_remaining); // Compute adjusted step rate inverse

		// Compute CPU cycles per step for the prepped segment.
		uint32_t cycles = ceil((TICKS_PER_MICROSECOND*1000000*60)*inv_rate); // (cycles/step)

		// Compute step timing and multi-axis smoothing level.
		// NOTE: AMASS overdrives the timer with each level, so only one prescalar is required.
		if(cycles < AMASS_LEVEL1) {
			prep_segment->amass_level = 0;
		}
		else {
			if(cycles < AMASS_LEVEL2) {
				prep_segment->amass_level = 1;
			}
			else if(cycles < AMASS_LEVEL3) {
				prep_segment->amass_level = 2;
			}
			else {
				prep_segment->amass_level = 3;
			}

			cycles >>= prep_segment->amass_level;
			prep_segment->n_step <<= prep_segment->amass_level;
		}

		if(cycles < (1UL << 16)) {
			// < 65536 (2.7ms @ 24MHz)
			prep_segment->cycles_per_tick = cycles;
		}
		else {
			// Just set the slowest speed possible.
			prep_segment->cycles_per_tick = 0xffff;
		}

		// Segment complete! Increment segment buffer indices, so stepper ISR can immediately execute it.
		segment_buffer_head = segment_next_head;
		if(++segment_next_head == SEGMENT_BUFFER_SIZE) {
			segment_next_head = 0;
		}

		// Update the appropriate planner and segment data.
		pl_block->millimeters = mm_remaining;
		prep.steps_remaining = n_steps_remaining;
		prep.dt_remainder = (n_steps_remaining - step_dist_remaining)*inv_rate;

		// Check for exit conditions and flag to load next planner block.
		if(mm_remaining == prep.mm_complete) {
			// End of planner block or forced-termination. No more distance to be executed.
			if(mm_remaining > 0.0) { // At end of forced-termination.
				// Reset prep parameters for resuming and then bail. Allow the stepper ISR to complete
				// the segment queue, where realtime protocol will set new state upon receiving the
				// cycle stop flag from the ISR. Prep_segment is blocked until then.
				BIT_TRUE(sys.step_control, STEP_CONTROL_END_MOTION);
#ifdef PARKING_ENABLE
				if(!(prep.recalculate_flag & PREP_FLAG_PARKING)) {
					prep.recalculate_flag |= PREP_FLAG_HOLD_PARTIAL_BLOCK;
				}
#endif
				return; // Bail!
			}
			else { // End of planner block
				// The planner block is complete. All steps are set to be executed in the segment buffer.
				if(sys.step_control & STEP_CONTROL_EXECUTE_SYS_MOTION) {
					BIT_TRUE(sys.step_control, STEP_CONTROL_END_MOTION);

					return;
				}

				pl_block = 0; // Set pointer to indicate check and load next planner block.
				Planner_DiscardCurrentBlock();
			}
		}
	}
}


// Called by realtime status reporting to fetch the current speed being executed. This value
// however is not exactly the current speed, but the speed computed in the last step segment
// in the segment buffer. It will always be behind by up to the number of segment blocks (-1)
// divided by the ACCELERATION TICKS PER SECOND in seconds.
float Stepper_GetRealtimeRate(void)
{
	if(sys.state & (STATE_CYCLE | STATE_HOMING | STATE_HOLD | STATE_JOG | STATE_SAFETY_DOOR)) {
		return prep.current_speed;
	}

	return 0.0f;
}
