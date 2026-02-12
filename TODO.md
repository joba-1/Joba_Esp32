# To Do

* receive modbus requests via mqtt

* bad request - response matching. Consider using number of registers never used by other master (might not be needed, since it works well now)

* Expose strict-window rejection counter in Modbus status/patterns API and increment it when responses are rejected for arriving >200 ms after the associated request. Include in `/api/modbus/status` and `/api/modbus/patterns` output.

