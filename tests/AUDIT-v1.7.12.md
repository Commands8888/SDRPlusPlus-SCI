# SDRPP-FULL v1.7.12 — continuidade do áudio e cadência do FFT

Base: `abd4e614cc21ab4a651a32b6b6f44176107ada59` (v1.7.11).
Correção dirigida ao áudio intermitente em todos os modos, preservando a
resposta de ganho/AGC, a troca de modos e os cálculos do LogMMSE.

## Reprodução e correção

A FIFO de saída comportava aproximadamente dois períodos do dispositivo.
Um backend que atende vários períodos no mesmo despertar pode pedir mais
amostras do que essa reserva comporta, apesar de manter a taxa média correta.
O callback então insere silêncio. Os testes anteriores aceitavam esse silêncio
se as amostras recebidas continuassem em ordem, deixando passar cortes audíveis.

A reprodução local, com 48 kHz, períodos de 800 frames e quatro callbacks por
ciclo de 66,7 ms, mostrou 44 callbacks incompletos e 25.344 frames de silêncio
no trecho observado da implementação anterior. Com a reserva corrigida, o
mesmo ensaio terminou com zero callbacks incompletos e zero frames ausentes.

A capacidade passa a cobrir quatro períodos, com mínimo de 50 ms, arredondada
para potência de dois. A reserva inicial cobre dois períodos, com mínimo de
20 ms e limite de metade da capacidade. Não existe espera da GUI nem espera
pelo DSP dentro do callback nativo; o preenchimento continua na thread produtora.

No exemplo de 48 kHz/800 frames, a capacidade passa de 2.048 para 4.096 frames
(42,7 para 85,3 ms), com mais 16 KiB por saída. A reserva inicial passa de 800
para 1.600 frames (16,7 para 33,3 ms). Esse é o custo explícito de margem contra
jitter; a fila mantém tamanho fixo. PCM, ganho, banda e decodificadores não são
modificados. A entrada RTL da 1.7.11 permanece assíncrona e sem espera pelo DSP.

## FFT e escopo

Áudio, VFO e FFT compartilham o splitter de IQ. Esperas e vazão irregular no
áudio podem afetar a entrega ao FFT; por isso o ensaio gráfico observa ambos
juntos, com entrada de 2,4 MHz, NR e dispositivo atendendo quatro períodos.

Foi acrescentada somente uma leitura protegida do índice de quadros FFT já
existente, sem novo estado ou buffer, para observar a cadência no teste. Tamanho/janelas da FFT,
smoothing, desenho, waterfall, colormaps, temas e bandplans permanecem iguais.
Essa leitura não modifica amostras ou parâmetros gráficos.

## Verificação

```sh
bash scripts/test_audio_flow.sh build memory
CXX=clang++ bash scripts/test_audio_flow.sh build status
bash scripts/test_radio_gui_modes.sh build
bash scripts/test_radio_gui_modes.sh build nr
bash scripts/test_radio_gui_modes.sh build continuity
```

- ASan/UBSan e TSan locais passaram no fluxo de áudio/IQ e na entrada RTL.
- A nova matriz de relógio cobre 44,1/48/96 kHz, mono/estéreo e atendimento de
  1/2/4 períodos por ciclo. Exige zero silêncio inserido no trecho estável e
  a sequência PCM exata, sem reordenar, repetir ou interpolar amostras.
- Os sinks reais RtAudio 5/6 e PortAudio são exercitados com APIs simuladas,
  incluindo quatro callbacks consecutivos, mono/estéreo, falta de dispositivo
  e falhas de abertura/start/stop. O callback continua sem alocar memória.
- A integração gráfica existente mantém as 880 seleções normais e 220 com NR,
  alterações de taxa de áudio e comandos concorrentes com o contexto USB.
- O cenário adicional mantém cada um dos onze modos por um intervalo fixo,
  separando aquecimento e medição. Exige continuidade PCM nos nove modos
  analógicos e acompanha FFT, comandos e perda de IQ em todos os onze modos.
- DSD/oldDSD recebem um sinal sintético sem quadros digitais válidos: não se
  exige fala decodificada nesse cenário. Os testes próprios dos decodificadores
  continuam ativos. Não se trata de validação de protocolos com rádio físico.
- A matriz de empacotamento conserva os oráculos numéricos do NR, verificações
  de AGC/filtros, abertura/reabertura/encerramento e isolamento da RTL privada.

Não há rádio ou placa de som física conectado ao ambiente. Os ensaios reproduzem
o defeito de reserva identificado e verificam continuidade com dispositivos
simulados; jitter fora da margem configurada ainda pode provocar underruns.
