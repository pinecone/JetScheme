"""Statistics and readable results for paired A/B benchmark runs."""

import math
import random
import statistics
import sys
from dataclasses import dataclass


def _beta_fraction(a, b, x):
    """Continued fraction for the incomplete beta function."""
    tiny = 1e-300
    qab = a + b
    qap = a + 1
    qam = a - 1
    c = 1.0
    d = 1 - qab * x / qap
    if abs(d) < tiny:
        d = tiny
    d = 1 / d
    h = d
    for m in range(1, 201):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1 / d
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1 + aa * d
        if abs(d) < tiny:
            d = tiny
        c = 1 + aa / c
        if abs(c) < tiny:
            c = tiny
        d = 1 / d
        step = d * c
        h *= step
        if abs(step - 1) < 3e-14:
            return h
    raise ArithmeticError("incomplete beta fraction did not converge")


def _regularized_beta(x, a, b):
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    factor = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
                      + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1) / (a + b + 2):
        return factor * _beta_fraction(a, b, x) / a
    return 1 - factor * _beta_fraction(b, a, 1 - x) / b


def paired_t_p(log_ratios):
    """Two-sided paired Student t-test for a mean log time ratio of zero."""
    n = len(log_ratios)
    if n < 2:
        raise ValueError("paired t-test needs at least two trials")
    mean = statistics.fmean(log_ratios)
    sd = statistics.stdev(log_ratios)
    if sd == 0:
        return 1.0 if mean == 0 else 0.0
    t = mean * math.sqrt(n) / sd
    df = n - 1
    return _regularized_beta(df / (df + t * t), df / 2, 0.5)


def holm_p(p_values):
    """Adjusted p-values controlling familywise error across all comparisons."""
    adjusted = [0.0] * len(p_values)
    largest = 0.0
    for rank, index in enumerate(sorted(range(len(p_values)), key=p_values.__getitem__)):
        largest = max(largest, min(1.0, (len(p_values) - rank) * p_values[index]))
        adjusted[index] = largest
    return adjusted


def percent_result(ratio, significant):
    """Describe the measured change without claiming an unclear result is real."""
    if abs(ratio - 1) < 1e-12:
        return "0% change" if significant else "unclear (no measured change)"
    amount = f"{abs((ratio - 1) * 100):.3g}%"
    direction = "faster" if ratio < 1 else "slower"
    change = f"{amount} {direction}"
    return change if significant else f"unclear ({change})"


def rival_percent(ratio):
    """Rival speed relative to Jet (jet time / rival time)."""
    amount = abs((ratio - 1) * 100)
    return f"{amount:.1f}%" if amount >= 0.05 else "<0.1%"


def show_progress(done, total, stream=None):
    """Redraw a single progress line on interactive terminals."""
    if stream is None:
        stream = sys.stdout
    if total == 0 or not stream.isatty():
        return
    filled = 24 * done // total
    bar = "█" * filled + "░" * (24 - filled)
    print(f"\r  {bar}  {done}/{total}", end="\n" if done == total else "",
          file=stream, flush=True)


@dataclass(frozen=True)
class PairedResult:
    log_ratios: tuple[float, ...]
    primary_times: tuple[float, ...]
    baseline_median: float

    @property
    def ratio(self):
        return math.exp(statistics.fmean(self.log_ratios))

    @property
    def p(self):
        return paired_t_p(self.log_ratios)


def collect(primary_cmd, baseline_cmd, runs, time_run, warmups=3, rng=None):
    """Time paired processes; randomize order, balanced within two-pair blocks."""
    if runs < 2 or warmups < 0:
        raise ValueError("runs must be at least two and warmups must be nonnegative")
    if rng is None:
        rng = random.Random()

    for i in range(warmups):
        first_primary = bool(rng.getrandbits(1)) if i == 0 else not first_primary
        if first_primary:
            time_run(primary_cmd)
            time_run(baseline_cmd)
        else:
            time_run(baseline_cmd)
            time_run(primary_cmd)

    log_ratios, primary_times, baseline_times = [], [], []
    for i in range(runs):
        if i % 2 == 0:
            first_primary = bool(rng.getrandbits(1))
        else:
            first_primary = not first_primary
        if first_primary:
            primary = time_run(primary_cmd)
            baseline = time_run(baseline_cmd)
        else:
            baseline = time_run(baseline_cmd)
            primary = time_run(primary_cmd)
        if primary <= 0 or baseline <= 0:
            raise ValueError("timed runs must have positive durations")
        log_ratios.append(math.log(primary) - math.log(baseline))
        primary_times.append(primary)
        baseline_times.append(baseline)
    return PairedResult(tuple(log_ratios), tuple(primary_times),
                        statistics.median(baseline_times))
