# SDRPP-FULL v1.7.12 — continuidade do áudio e cadência do FFT

Base: `abd4e614cc21ab4a651a32b6b6f44176107ada59` (v1.7.11).
Correção do áudio intermitente, preservando ganho/AGC, troca de modos e NR.

## Causa reproduzida

A saída iniciava com uma reserva dimensionada só pelo período do dispositivo,
sem cobrir os blocos entregues pelo processamento. O Audio NR2, por exemplo,
publica blocos de até 4.608 frames em 48 kHz (96 ms). Callbacks agrupados podem
consumir a reserva antes do próximo bloco e inserir silêncio. Os testes
anteriores aceitavam esse silêncio se as amostras mantivessem a ordem.

Com 48 kHz/800 frames e quatro callbacks por ciclo de 66,7 ms, a implementação
anterior apresentou 44 callbacks incompletos e 25.344 frames de silêncio no
trecho observado. O teste conjunto de modos também identificou cortes residuais
quando a preparação não considerava o tamanho dos blocos do NR.

## Política de saída

- O produtor observa o tamanho do pacote PCM recebido, sem modificar amostras.
- Ao começar ou recuperar a reprodução, a reserva considera esse pacote e uma
  rajada de quatro períodos do dispositivo, limitada ao armazenamento disponível.
- O armazenamento é pré-alocado: quatro períodos mais 125 ms de margem,
  arredondados para potência de dois. Não cresce durante a reprodução.
- O callback nativo continua sem esperar pelo DSP, sem alocar memória e sem
  acessar estado da GUI. Pacotes curtos finitos mantêm entrega após espera limitada.

Em 48 kHz/800 frames, o armazenamento é de 16.384 frames (128 KiB): mais
112 KiB por saída em relação à 1.7.11. Capacidade não é uma espera obrigatória:
com um pacote de 480 frames, a preparação é de 3.680 frames (76,7 ms); com o
bloco NR de 4.608 frames, é de 7.808 frames (162,7 ms). Essa margem cobre a
entrega em blocos sem reduzir a janela ou alterar os cálculos do NR.

O RtAudio/PulseAudio recebe um pedido explícito de dois períodos no buffer
nativo. No RtAudio 5.2.0 do Noble, `numberOfBuffers` limita
`pa_buffer_attr.maxlength`; somente `MINIMIZE_LATENCY` deixa os padrões do
servidor nesse backend. ALSA e os demais backends mantêm suas opções anteriores.
Referência: [RtAudio 5.2.0](https://github.com/thestk/rtaudio/blob/5.2.0/RtAudio.cpp),
`RtApiPulse::probeDeviceOpen`.

O caminho assíncrono RTL da 1.7.11 permanece intacto. A biblioteca RTL privada,
faixa de ganho, filtros, banda, protocolos e cálculos DSP não foram alterados.

## FFT

O splitter de IQ é compartilhado por áudio, VFO e FFT; a cadência é observada
junto com a continuidade do áudio. Foi acrescentada apenas uma leitura protegida
do índice de quadros já existente, sem novo estado ou buffer gráfico. Tamanho,
janelas, smoothing, desenho, waterfall, colormaps e bandplans permanecem iguais.

## Verificação

```sh
bash scripts/test_audio_flow.sh build memory
CXX=clang++ bash scripts/test_audio_flow.sh build status
bash scripts/test_radio_gui_modes.sh build
bash scripts/test_radio_gui_modes.sh build nr
bash scripts/test_radio_gui_modes.sh build continuity
```

- Build Noble e ASan/UBSan/TSan locais passaram nos fluxos de áudio/IQ/RTL.
- Relógio independente em mono/estéreo, 8/16/24/44,1/48/96 kHz, pacotes curtos
  ou do tamanho usado pelo NR e atendimento de 1/2/4 períodos: zero silêncio
  inserido após começar a entrega contínua; sequência PCM exata.
- Sinks reais RtAudio 5/6 e PortAudio com APIs simuladas: quatro callbacks
  consecutivos, mono/estéreo, falta de dispositivo e erros de open/start/stop.
  Verificam também a configuração do PulseAudio e ausência de alocações no callback.
- Permanecem as 880 seleções normais e 220 com NR, taxas de áudio diferentes
  e comandos concorrentes com o contexto USB.
- O cenário conjunto usa IQ de 2,4 MHz com NR, aquece cada modo e mede mais
  2,5 segundos: exige continuidade PCM nos nove modos analógicos e acompanha
  FFT, comandos e perda de IQ nos onze modos. Registra os tamanhos dos pacotes.
- DSD/oldDSD recebem sinal sintético sem quadros digitais válidos: não se exige
  fala decodificada nesse cenário. Seus testes próprios permanecem ativos.
- A matriz conserva oráculos numéricos do NR, AGC/filtros, ciclo de vida dos
  pacotes e isolamento da RTL privada.

Não há rádio ou placa de som física conectado ao ambiente. Os ensaios reproduzem
o defeito identificado com dispositivos simulados. A margem é limitada; uma
máquina que não sustente o processamento ainda pode apresentar underruns.
