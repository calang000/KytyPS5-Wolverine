# Bounded autonomous diagnostic input

The existing `KYTY_TEST_PAD_FILE` hook also accepts `walk x y milliseconds`,
where x/y are -1,0,1 and duration is100..10000ms. It releases both left-stick
axes automatically. `attack` and `dodge` hold Square/Circle for300ms, like
the existing Cross/Options/D-pad commands. `quit` takes the regular SDL quit
event path, allowing final pipeline-cache state to be saved.

Only one synthetic input is held at a time. The entire hook is inactive unless
an explicit command-file path was supplied. Public play launchers clear it.
Automated movement/combat checks use a copied protected diagnostic save and
their samples are separate from stationary A/B performance measurements.
