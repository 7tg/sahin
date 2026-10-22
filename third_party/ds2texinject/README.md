# DS2TexInject

`ds2texinject.cpp` and `LICENSE` are DS2TexInject 1.3.0 by Sidnei Alves de Almeida (MIT),
from https://github.com/sidnei-almeida/dead-space-2-texmod-linux at
commit f10fd12891095ebf106fccdbda272c1f3388fa61 (`src/ds2texinject.cpp`), unchanged.

It is an ASI plugin that hooks the game's Direct3D 9 device and swaps textures by their
TexMod hash (CRC-32 of the first level). Made for Dead Space 2, it works with any Direct3D 9
game. Sahin builds it (`tools/build-prompts.sh`) to give Blur PlayStation button prompts: its
settings are `profile/blur/DS2TexInject.ini`, the texture `art/prompts.png`.
