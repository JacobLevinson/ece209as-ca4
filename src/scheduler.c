#include <stdio.h>
#include "utlist.h"
#include "utils.h"
#include "params.h"

#include "memory_controller.h"

extern long long int CYCLE_VAL;

// write queue high water mark; begin draining writes if write queue exceeds this value
#define HI_WM 64

// end write queue drain once write queue has this many writes in it
#define LO_WM 36

#define ROW_IDLE 72 // cycles a row may stay open with no hits

// 1 means we are in write-drain mode for that channel
int drain_writes[MAX_NUM_CHANNELS];

long long int last_col_cycle[MAX_NUM_CHANNELS][MAX_NUM_RANKS][MAX_NUM_BANKS];

void init_scheduler_vars()
{
	// initialise timeout table
	for (int channel = 0; channel < MAX_NUM_CHANNELS; channel++)
		for (int r = 0; r < MAX_NUM_RANKS; r++)
			for (int b = 0; b < MAX_NUM_BANKS; b++)
				last_col_cycle[channel][r][b] = -1;
}

// helper: does any queue entry still hit the given open row?
static int other_hit_exists(int channel, int rank, int bank, long long int row)
{
	request_t *req;
	LL_FOREACH(read_queue_head[channel], req)
	if (req->dram_addr.rank == rank &&
	    req->dram_addr.bank == bank &&
	    req->dram_addr.row == row)
		return 1;
	LL_FOREACH(write_queue_head[channel], req)
	if (req->dram_addr.rank == rank &&
	    req->dram_addr.bank == bank &&
	    req->dram_addr.row == row)
		return 1;
	return 0;
}

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
	request_t *req = NULL;
	request_t *best_req = NULL;

	// write-drain bookkeeping
	if (drain_writes[channel] && write_queue_length[channel] > LO_WM)
		drain_writes[channel] = 1;
	else
		drain_writes[channel] = 0;

	if (write_queue_length[channel] > HI_WM ||
	    !read_queue_length[channel])
		drain_writes[channel] = 1;

	request_t **head = drain_writes[channel] ? &write_queue_head[channel] : &read_queue_head[channel];

	// pass 1 : pick oldest row-hit that is issuable
	LL_FOREACH(*head, req)
	{
		if (!req->command_issuable)
			continue;
		if (req->next_command == COL_READ_CMD ||
		    req->next_command == COL_WRITE_CMD)
		{
			best_req = req;
			break; // first issuable hit wins
		}
	}

	// fallback to FCFS)
	if (!best_req)
	{
		LL_FOREACH(*head, req)
		if (req->command_issuable)
		{
			best_req = req;
			break;
		}
	}

	// issue chosen command
	if (best_req)
	{
		int ch = channel;
		int rank = best_req->dram_addr.rank;
		int bank = best_req->dram_addr.bank;
		long long row = best_req->dram_addr.row;
		int is_col = (best_req->next_command == COL_READ_CMD ||
			      best_req->next_command == COL_WRITE_CMD);

		issue_request_command(best_req);

		// remember last column activity for timeout
		if (is_col)
			last_col_cycle[ch][rank][bank] = CYCLE_VAL;

		// auto-precharge if this was the last hit to that row
		if (is_col &&
		    !other_hit_exists(ch, rank, bank, row) &&
		    is_autoprecharge_allowed(ch, rank, bank))
			issue_autoprecharge(ch, rank, bank);

		return; // exactly one command per cycle
	}

	// idle: consider timing-out an open row
	if (!command_issued_current_cycle[channel])
	{
		for (int r = 0; r < NUM_RANKS; r++)
		{
			for (int b = 0; b < NUM_BANKS; b++)
			{
				bank_t *bs = &dram_state[channel][r][b];
				if (bs->state == ROW_ACTIVE &&
				    last_col_cycle[channel][r][b] >= 0 &&
				    (CYCLE_VAL - last_col_cycle[channel][r][b]) > ROW_IDLE &&
				    !other_hit_exists(channel, r, b, bs->active_row) &&
				    is_precharge_allowed(channel, r, b))
				{
					issue_precharge_command(channel, r, b);
					// count only one precharge per idle cycle
					return;
				}
			}
		}
	}
}

void scheduler_stats()
{
  /* Nothing to print for now. */
}

