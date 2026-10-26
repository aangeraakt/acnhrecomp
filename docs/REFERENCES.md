# O que aprendemos com cada projeto de referência

Quatro projetos de recompilação/reconstrução de jogos de Switch foram estudados antes de começar. Este é o resumo
do que cada um faz e do que foi aproveitado aqui.

## SuperMarioWonderRecomp: a base

<https://github.com/BelmanteGu/SuperMarioWonderRecomp> (MIT). Super Mario Bros. Wonder 1.0.0 (SDK 16) e Hades.

- **Recompilador em Python** (`recomp/`): carrega o NSO (LZ4, MOD0, `.dynamic`, relocações), acha as funções pelo
  `.eh_frame_hdr` (FDEs) e pelos alvos de `bl`, relocações RELATIVE, init_array, exports e ponteiros `adr`;
  resolve tabelas de salto (as de clang e as `ldrsw` + `add`), e emite uma função C++ por função do jogo. O
  contexto `nx::CPU` guarda x0-x30, SP, v0-v31, NZCV, FPCR/FPSR e o monitor exclusivo.
- **Teste diferencial** (`tests/difftest.py`): cada *forma* de instrução do módulo roda nativa e no Unicorn, com
  estados aleatórios, e os registradores e a memória são comparados.
- **Runtime em C++**: rtld (relocações por nome, HLE primeiro), kernel Horizon (threads, ArbitrateLock, condvars,
  WaitForAddress, eventos, memória), IPC HIPC/CMIF com domínios, `sm:` e serviços; HLE de `nn::fs`, `nn::hid`,
  `nn::audio` (o AudioRenderer do próprio SDK roda em processo), `nn::pl`, `nn::vi`, `nn::nifm`/`nn::socket`.
- **NVN → Vulkan**: `nvnBootstrapLoader` e `nvnDeviceGetProcAddress` devolvem implementações do host; command
  buffers gravados e reproduzidos em Vulkan; pools de memória importados com `VK_EXT_external_memory_host`;
  destiling block-linear; ASTC/BCn; pipelines com dynamic state e cache em disco; FSR 1.
- **Shaders**: extraídos de BNSH/FSHA/BAHS/VFXB e traduzidos **offline** para SPIR-V com o tradutor do Ryujinx
  (MIT), chaveados pelo hash do bloco de código.

Aproveitado: praticamente tudo. O ACNH é outro jogo first-party NVN com o mesmo formato de módulos, então o
framework serve quase direto; o trabalho foi o que muda com o SDK 20 e com o motor do ACNH (veja `ACNH.md`).

## LASRecomp

<https://github.com/peiikai/LASRecomp> (MIT). The Legend of Zelda: Link's Awakening, macOS/Windows/Linux.

- **Lifter** em Python (`tools/lifter.py`, `tools/gen.py`) para C, com SIMD em asm (ARM) ou C portátil
  (`tools/simd_c.py`). Tabelas de salto resolvidas executando a sequência no Unicorn para cada índice.
- **Despacho** com cache por thread (4.096 entradas) sobre busca binária; `musttail` para desvios.
- **Recompilação na primeira execução** (`runtime/gamebuild.c`): o app distribuído não contém código do jogo; o
  gerador roda no PC do jogador e compila com `zig cc` em paralelo. O código do jogo fica numa biblioteca
  separada, o que permite reiniciar sem fechar o app.
- **NVN** próprio: tradutor Maxwell → MSL → GLSL → SPIR-V (shaderc) com cache.
- **Relógio** (`services/time.c`): `time:u/a/s` a partir de `time()`/`localtime_r`, shared memory do relógio, e
  HLE de `StandardUserSystemClock::GetCurrentTime` direto do host. Para o ACNH é a parte mais sensível: o jogo
  inteiro anda no relógio do sistema.
- **Saves** em pastas do host, **fontes compartilhadas** (cadeia de fallback: variável, fontes do console do
  usuário decodificadas, fontes do sistema) e **teclado** (HLE de `nn::swkbd::ShowKeyboard` com overlay ImGui).

Aproveitado: as ideias de relógio direto do host e de fallback de fontes (o runtime do Wonder já segue a mesma
linha). O ACNH não usa o applet swkbd para nomes: ele traz o próprio teclado (iWnn) na RomFS.

## SwitchRecomp

<https://github.com/bgyss/SwitchRecomp>. Conjunto de **especificações** e um esqueleto em Rust; não roda jogos
comerciais. Úteis: `specs/SPEC-070-OS-SERVICES.md` (diagramas byte a byte de HIPC/CMIF/TIPC, `sm:` em TIPC desde
o firmware 12), `SPEC-170` (descoberta determinística de funções) e `SPEC-190/220/240` (validação por replay de
entrada e comparação de vídeo/hash), um bom modelo para testes de regressão.

## uDPR

<https://github.com/shatter9652/uDPR>. Pokémon Brilliant Diamond reconstruído como **projeto Unity**: o C# do
jogo (IL2CPP) foi refeito a partir dos metadados e os assets são exportados do dump do usuário com AssetRipper.
As chamadas `nn::*` viram shims em C#. Não há tradução de binário; não se aplica ao ACNH (C++ nativo com NVN),
mas a lista de shims mostra quais subsistemas do Switch um jogo costuma tocar, e o setup em etapas retomáveis
inspirou `tools/setup_acnh.sh`.
