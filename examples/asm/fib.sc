// A DOS-style program: it prints into the SolarOS terminal (like the serial
// port) and does not take over the screen.
//   cd /flash
//   asm run fib.sc        the table fib(0) .. fib(30)
//   asm run fib.sc 40     only fib(40), with the time it took
// The command-line parameter arrives as main's argument (n); argc says how
// many were given, arg(i) / argf(i) read them anywhere in the script.
int fib(int n)
{
   if (n < 2)
      return n;
   return fib(n - 1) + fib(n - 2);
}

int main(int n)
{
   if (argc > 0)
   {
      tprintln("computing fib(%d)...", n);
      uint32_t start = millis();
      int value = fib(n);
      tprintln("fib(%d) = %d   (%d ms)", n, value, millis() - start);
      return 0;
   }
   tprintln("Fibonacci, computed by ESPLiveScript on the ESP32-S3");
   tprintln("--------------------------------------------------");
   for (int i = 0; i <= 30; i++)
   {
      uint32_t start = millis();
      int value = fib(i);
      uint32_t took = millis() - start;
      tprintf("fib(%d) = ", i);
      tprintln("%d   (%d ms)", value, took);
   }
   println("done");
   return 0;
}
