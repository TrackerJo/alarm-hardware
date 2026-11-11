import Foundation

// Generate a regression trace using the existing app's actual model/encoder.
// This does not modify or rebuild the iOS application.
@main
struct MakeDawnTrace {
    static func main() throws {
        NSTimeZone.default = TimeZone(identifier: "America/Indiana/Indianapolis")!
        let now = ISO8601DateFormatter().date(from: "2026-10-29T11:00:00Z")!
        var alice = WakeAlarm(ownerID: "alice", ownerName: "Alice", hour: 7, minute: 30, weekdays: [2, 4, 6])
        alice.setTime(WakeTime(hour: 7, minute: 31), on: [4])
        let bob = WakeAlarm(ownerID: "bob", ownerName: "Bob", hour: 8, minute: 15, weekdays: [1, 3, 5])
        print(try LegacySchedule.commands([alice, bob], now: now).joined(separator: "\n"))
    }
}
