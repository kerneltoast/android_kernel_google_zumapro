/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Perf metrics
 *
 * Copyright 2022 Google LLC
 */

#include <linux/interrupt.h>

#define RESUME_LATENCY_STEP_SMALL 10
#define RESUME_LATENCY_STEP_MID 50
#define RESUME_LATENCY_STEP_LARGE 100

#define RESUME_LATENCY_BOUND_SMALL 250
#define RESUME_LATENCY_BOUND_MID 500
#define RESUME_LATENCY_BOUND_MAX 1000

#define RESUME_LATENCY_DEFAULT_THRESHOLD 200

#define MAX_IRQ_NUM 2048
#define IRQ_ARR_LIMIT 100

#define RT_RUNNABLE_ARR_SIZE 5

#define LATENCY_CNT_SMALL (RESUME_LATENCY_BOUND_SMALL / RESUME_LATENCY_STEP_SMALL)
#define LATENCY_CNT_MID ((RESUME_LATENCY_BOUND_MID - RESUME_LATENCY_BOUND_SMALL) / \
	RESUME_LATENCY_STEP_MID)
#define LATENCY_CNT_LARGE ((RESUME_LATENCY_BOUND_MAX - RESUME_LATENCY_BOUND_MID) / \
	RESUME_LATENCY_STEP_LARGE)
#define RESUME_LATENCY_ARR_SIZE (LATENCY_CNT_SMALL + LATENCY_CNT_MID + LATENCY_CNT_LARGE + 1)