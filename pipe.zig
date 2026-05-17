const std = @import("std");

pub const ParseError = error{
    InvalidAddress,
    BufferTooSmall,
};

pub fn inetAton(str: []const u8) ParseError!u32 {
    var iter = std.mem.splitScalar(u8, str, '.');

    var parts: [4]u8 = undefined;
    var i: usize = 0;

    while (iter.next()) |octet| {
        if (i >= 4)
            return error.InvalidAddress;

        if (octet.len > 1 and octet[0] == '0')
            return error.InvalidAddress;

        const value = std.fmt.parseInt(u8, octet, 10)
            catch return error.InvalidAddress;

        parts[i] = value;
        i += 1;
    }

    if (i != 4) return error.InvalidAddress;

    return (@as(u32, parts[0]) << 24) |
           (@as(u32, parts[1]) << 16) |
           (@as(u32, parts[2]) << 8)  |
           (@as(u32, parts[3]));
}

pub fn inetNtoa(addr: u32, buf: []u8) ParseError![]const u8 {
    return std.fmt.bufPrint(
        buf,
        "{d}.{d}.{d}.{d}",
        .{
            (addr >> 24) & 0xff,
            (addr >> 16) & 0xff,
            (addr >> 8) & 0xff,
            addr & 0xff,
        },
    ) catch error.BufferTooSmall;
}

pub fn inetPton6(str: []const u8) ParseError![16]u8 {
    const addr = std.net.Address.parseIp6(str, 0)
        catch return error.InvalidAddress;

    return addr.in6.sa.addr;
}
