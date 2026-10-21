"""Monotonic, cumulative phase deadlines for the D6 allocation."""
import time

CAPS = {'resume': 180, 'prepare': 260, 'verify': 635, 'cleanup': 90, 'stop': 480}
RESERVE = {'resume': 570, 'prepare': 570, 'verify': 570, 'cleanup': 480, 'stop': 0}


class Exhausted(RuntimeError):
    pass


class Budget:
    def __init__(self, clock=time.monotonic, seconds=1800):
        self.clock = clock
        self.started = clock()
        self.deadline = self.started + seconds

    def phase(self, name):
        now = self.clock()
        end = min(now + CAPS[name], self.deadline - RESERVE[name])
        if now >= end:
            raise Exhausted(f'{name}: deadline/reservation exhausted before phase start')
        return Phase(self, name, now, end)


class Phase:
    def __init__(self, budget, name, started, deadline):
        self.budget, self.name = budget, name
        self.started, self.deadline = started, deadline

    def remaining(self):
        return self.deadline - self.budget.clock()

    def timeout(self, requested):
        left = self.remaining()
        if left <= 0:
            raise Exhausted(f'{self.name}: cumulative deadline exhausted; no new command')
        return min(requested, left)

    def pause(self, seconds):
        time.sleep(self.timeout(seconds))
