	struct sigaction action_settings = {
		.sa_sigaction = &signal_handler,
		.sa_flags = SA_SIGINFO
	};
	sigemptyset(&action_settings.sa_mask);
	if (sigaction(SIGALRM, &action_settings, NULL) == -1) {
		perror("sigaction");
		goto out;
	}

	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGALRM);
	if (sigprocmask(SIG_SETMASK, &signal_mask, NULL) == -1) {
		perror("sigprocmask");
	}

	struct sigevent signal_settings = {
		.sigev_notify = SIGEV_SIGNAL,
		.sigev_signo = SIGALRM,
		//.sigev_value = {.sival_ptr = &test_timer}
		.sigev_value.sival_int = 12
	};
	timer_t timer;
	if (timer_create(CLOCK_REALTIME, &signal_settings, &timer) == -1) {
		perror("timer_create");
	}
	struct itimerspec timer_settings = {
		.it_interval = {0, 0},
		.it_value = {3, 0}
	};
	if (timer_settime(timer, 0, &timer_settings, NULL) == -1) {
		perror("timer_settime");
	}
	if (sigprocmask(SIG_UNBLOCK, &signal_mask, NULL) == -1) {
		perror("sigprocmask");
	}
