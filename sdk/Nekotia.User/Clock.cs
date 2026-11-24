namespace Nekotia.User;

/// <summary>A calendar date and time of day.</summary>
public struct DateTime
{
    public int Year, Month, Day, Hour, Minute, Second;
    /// <summary>0 = Monday … 6 = Sunday.</summary>
    public int WeekDay;
}

/// <summary>The wall clock: the CMOS real-time clock read at boot plus the uptime (system call 49), local time.</summary>
public static class Clock
{
    /// <summary>Seconds since 1970-01-01 00:00 (local time); 0 when the machine has no readable clock.</summary>
    public static long Now() => Process.Syscall(49);

    public static DateTime ToDate(long seconds)
    {
        DateTime t = default;
        long days = seconds / 86400;
        long rest = seconds % 86400;
        if (rest < 0) {
            rest += 86400;
            days--;
        }
        t.Hour = (int)(rest / 3600);
        t.Minute = (int)(rest / 60 % 60);
        t.Second = (int)(rest % 60);
        t.WeekDay = (int)((days % 7 + 7 + 3) % 7);   // 1970-01-01 was a Thursday
        // civil_from_days (Howard Hinnant)
        long z = days + 719468;
        long era = (z >= 0 ? z : z - 146096) / 146097;
        long doe = z - era * 146097;
        long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long y = yoe + era * 400;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        long mp = (5 * doy + 2) / 153;
        long d = doy - (153 * mp + 2) / 5 + 1;
        long m = mp < 10 ? mp + 3 : mp - 9;
        t.Year = (int)(y + (m <= 2 ? 1 : 0));
        t.Month = (int)m;
        t.Day = (int)d;
        return t;
    }

    public static DateTime Today() => ToDate(Now());

    public static string MonthName(int month) => month switch
    {
        1 => "January", 2 => "February", 3 => "March", 4 => "April", 5 => "May", 6 => "June",
        7 => "July", 8 => "August", 9 => "September", 10 => "October", 11 => "November", 12 => "December",
        _ => "?",
    };

    public static string DayName(int weekDay) => weekDay switch
    {
        0 => "Monday", 1 => "Tuesday", 2 => "Wednesday", 3 => "Thursday", 4 => "Friday", 5 => "Saturday", 6 => "Sunday",
        _ => "?",
    };
}
