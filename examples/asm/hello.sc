// A text parameter: asm run hello.sc Yves 3
// A parameter that is not a number arrives as a string (char *); the
// numbers as int or float. argc, arg(i), argf(i) and argstr(i) read them too.
void main(char *name, int times)
{
   if (argc < 1)
      name = "world";
   if (argc < 2)
      times = 1;
   for (int i = 1; i <= times; i++)
      tprintln("%d: hello %s", i, name);
}
