# To Do

* receive modbus requests via mqtt

* bad request - response matching. Consider using number of registers never used by other master (might not be needed, since it works well now)

* Do more unit tests
  * violate test name convention: keep filename cases intact
  * make a list of each cpp file to test (all in src)
  * for each file...
    * split functions into smaller ones if they contain many if's (consider if more than 6)
    * split out arduino agnostic helper functions (add _helper to the src cpp name)
    * write tests for all helpers to be run on native host
    * write tests that run on Esp32 for all functions and public methods, such that all code paths (also in used private methods) are passed at least once
  * don't directly test private methods. 
    * Assumption is, all code is useful, i.e. reachable via public functions
    * let ai help find parameters for public functions to reach code paths in private functions
  * evaluate tools to test coverage

