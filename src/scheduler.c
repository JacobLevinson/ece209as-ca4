#include <stdio.h>
#include "utlist.h"
#include "utils.h"

#include "memory_controller.h"

extern long long int CYCLE_VAL;

void init_scheduler_vars()
{
	// initialize all scheduler variables here

	return;
}

// write queue high water mark; begin draining writes if write queue exceeds this value
#define HI_WM 40

// end write queue drain once write queue has this many writes in it
#define LO_WM 20

// 1 means we are in write-drain mode for that channel
int drain_writes[MAX_NUM_CHANNELS];

/* Each cycle it is possible to issue a valid command from the read or write queues
   OR
   a valid precharge command to any bank (issue_precharge_command())
   OR 
   a valid precharge_all bank command to a rank (issue_all_bank_precharge_command())
   OR
   a power_down command (issue_powerdown_command()), programmed either for fast or slow exit mode
   OR
   a refresh command (issue_refresh_command())
   OR
   a power_up command (issue_powerup_command())
   OR
   an activate to a specific row (issue_activate_command()).

   If a COL-RD or COL-WR is picked for issue, the scheduler also has the
   option to issue an auto-precharge in this cycle (issue_autoprecharge()).

   Before issuing a command it is important to check if it is issuable. For the RD/WR queue resident commands, checking the "command_issuable" flag is necessary. To check if the other commands (mentioned above) can be issued, it is important to check one of the following functions: is_precharge_allowed, is_all_bank_precharge_allowed, is_powerdown_fast_allowed, is_powerdown_slow_allowed, is_powerup_allowed, is_refresh_allowed, is_autoprecharge_allowed, is_activate_allowed.
   */

void schedule(int channel)
{
	request_t *req = NULL;	    // iterator
	request_t *best_req = NULL; // what we’ll finally issue


	//  Update drain_writes[channel] state (same logic as baseline)
	if (drain_writes[channel] && (write_queue_length[channel] > LO_WM))
		drain_writes[channel] = 1; // stay draining
	else
		drain_writes[channel] = 0; // stop draining

	if (write_queue_length[channel] > HI_WM) // queue too full   
		drain_writes[channel] = 1;	 // start drain    
	else if (!read_queue_length[channel])	 // no reads pending 
		drain_writes[channel] = 1;	 // drain writes

	// Choose which queue we will look at this cycle

	request_t **head = drain_writes[channel] ? &write_queue_head[channel] : &read_queue_head[channel];

	// Pass 1 – find the oldest row-buffer hit whose command is issuable right now

	LL_FOREACH(*head, req)
	{
		if (!req->command_issuable)
			continue;

		if (req->next_command == COL_READ_CMD ||
		    req->next_command == COL_WRITE_CMD)
		{
			best_req = req; // first issuable hit wins
			break;
		}
	}

	// Pass 2: if no row-hit, fall back to oldest issuable request

	if (!best_req)
	{
		LL_FOREACH(*head, req)
		{
			if (req->command_issuable)
			{
				best_req = req; // first legal cmd in list
				break;
			}
		}
	}

	//  Issue the chosen command (if any)
	if (best_req)
	{
		issue_request_command(best_req);
		return; // exactly one cmd per cycle
	}

	// Nothing issuable → remain idle this cycle (NOP)
}

void scheduler_stats()
{
  /* Nothing to print for now. */
}

