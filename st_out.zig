const std = @import("std");

pub fn main() !void {
    const stdin = std.io.getStdIn().reader();
    const stdout = std.io.getStdOut().writer();

    try stdout.print("Input: ", .{});

    var input_buffer: [256]u8 = undefined; // fixed buffer for input
    const input = try stdin.readUntilDelimiterOrEof(&input_buffer, '\n');

    try stdout.print("\n : ", .{});

    for (input) |c| {
        // Print each character as 8-bit binary
        try stdout.print("{b:0>8} ", .{c});
    }

    try stdout.print("\n", .{});
}
