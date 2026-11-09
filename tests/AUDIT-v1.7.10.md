# SDRPP-FULL v1.7.10 — LogMMSE, filas IQ e saída de áudio

Base: `9a886d2fe61ace924e4db701a969e454b75db3c8` (v1.7.9).
Revisão motivada por picos com IQ de 2,4 MHz e pelo caminho dos onze modos
até a saída de áudio. Recursos visuais, bandplans, filtros, AGC, decodificadores
e biblioteca RTL privada foram preservados.

## LogMMSE

- **Histórico:** os vetores dos quadros que saem do histórico são reutilizados
  para os seguintes. O número de quadros, sua resolução e os cálculos das
  somas/desvios permanecem. O mesmo buffer é reutilizado enquanto o perfil
  está congelado pelo evento TX.
- **Modelo de ruído:** a nova estimativa é construída em buffers persistentes;
  só substitui a anterior quando a interpolação tem pontos válidos. Evita a
  cópia integral feita apenas para restaurar o modelo em caso de falha.
- **FFT:** planos com o mesmo tamanho são reutilizados após reset/retune.
  O tamanho da FFT, janela, sobreposição, tabelas E1/expfast e limites do
  estimador mantêm os valores da versão anterior. Não foi usado fast-math,
  reduzida a taxa IQ ou cortada a banda para obter os resultados.
- **Publicação:** o DSP escreve diretamente no buffer fixo do stream, um hop
  por vez (aproximadamente 10 ms de sinal). Elimina o vetor de saída e sua
  cópia, e distribui o backlog entre as chamadas. Isso limita a quantidade
  publicada; não promete um limite absoluto de tempo de CPU por chamada.
- **Cadência do modelo:** a atualização acompanha cada hop, em vez de depender
  de quantos hops vieram juntos em um pacote. O resultado deixa de depender
  do tamanho escolhido pelo driver para entregar os pacotes. Essa correção
  pode alterar a adaptação em relação à v1.7.9 quando ela recebia vários hops
  juntos; não altera a resolução ou os parâmetros do algoritmo.

## Entrada IQ

A fila de 32 quadros distinguia vazio e cheio apenas pelos índices módulo 32.
Ao encher, o produtor podia sobrescrever quadros ainda não consumidos e a fila
parecia vazia. Há agora contagem explícita sob o mesmo mutex, espera por espaço
no produtor e sinais de parada sincronizados. Os quadros são mantidos em ordem,
sem descarte silencioso por volta do índice. Flush e restart limpam os quadros
anteriores; a troca de bypass para os trabalhadores antes de alterar o caminho.
A capacidade permanece em 32 quadros, sem uma fila que cresça indefinidamente.

## Dispositivo de áudio

RtAudio e New PortAudio passam a usar a mesma FIFO estéreo, com produtor DSP
separado do callback do dispositivo. Os callbacks:

- não esperam pelo DSP, não usam mutex/condition variable e não alocam memória;
- aceitam tamanhos diferentes a cada chamada e preservam a ordem dos frames;
- inicializam a parte sem dados com silêncio, sem copiar amostras antigas ou
  memória não inicializada;
- preservam os dois canais em estéreo; em dispositivo mono usam a média L/R.

A fila é dimensionada para aproximadamente dois períodos do dispositivo, com
mínimo de 20 ms, arredondada para potência de dois. Com 48 kHz e período de
800 frames, comporta 2.048 frames (42,7 ms). A primeira reprodução/reentrada
aguarda uma pequena reserva: aproximadamente 10 ms ou um período do dispositivo,
o que for maior, limitada pela capacidade da fila. Um último pacote curto
também é entregue após uma espera limitada. A espera quando a fila enche ocorre no produtor, não no
callback. Não há espera fixa na inicialização da interface do aplicativo.

O New PortAudio deixa de depender do estado da GUI dentro do callback e do
handler que marcava o leitor como parado ao iniciar a recepção. Falha de
abertura/start e ausência de dispositivo mantêm um null sink drenando o
stream, evitando bloquear o splitter/FFT. RtAudio passa a aceitar dispositivos
mono e a tratar erros de parada/fechamento; seu callback de erro da API 6 não
propaga exceções para fora da thread nativa.

## Medições locais comparáveis

Noble, GCC 13, `-O3`, mesma entrada determinística, 500 hops por taxa. Os 220
primeiros aquecem o histórico; os 280 seguintes são medidos. Mediana de três
execuções intercaladas v1.7.9/nova implementação, com relógio de CPU da thread.

| IQ | CPU, ms por segundo de sinal, antes → depois | p95 por hop, µs, antes → depois | p99 por hop, µs, antes → depois |
|---|---:|---:|---:|
| 192 kHz | 18,767 → 17,836 | 259,6 → 241,6 | 325,2 → 383,7 |
| 1 MHz | 112,107 → 104,082 | 1.339,7 → 1.249,6 | 2.274,4 → 1.465,6 |
| 2,4 MHz | 268,521 → 259,623 | 3.161,0 → 3.095,6 | 3.614,1 → 3.494,0 |

O ganho adicional de CPU foi aproximadamente 3,3% em 2,4 MHz e 7,2% em 1 MHz
neste teste. Há variação de escalonamento nos percentis, inclusive uma subida
do p99 a 192 kHz. Não se trata de uma promessa de queda equivalente no uso
total do aplicativo, especialmente com DSD e renderização da interface ativos.

Depois do aquecimento, as alocações C++ caíram de 7,1 para 0,1 por hop. Em
2,4 MHz, a solicitação de memória temporária passou de aproximadamente
2.496.049 para 49 bytes por hop; restam pequenas alocações dos nós de deque.
Esses valores são tráfego de alocação, não RAM residente. Os hashes das
amostras no benchmark de um hop por chamada coincidiram nas três taxas.

## Verificação

```sh
bash scripts/test_audio_flow.sh build memory
CXX=clang++ bash scripts/test_audio_flow.sh build status
bash scripts/test_noise_reduction.sh build memory
CXX=clang++ bash scripts/test_noise_reduction.sh build status
bash scripts/test_noise_reduction.sh build flow-benchmark
bash scripts/test_radio_gui_modes.sh build
bash scripts/test_radio_gui_modes.sh build nr
```

- ASan/UBSan e TSan na FIFO de áudio e na fila IQ: ordem das amostras, mono e
  estéreo, 44,1/48/96 kHz, pacotes variáveis, silêncio, fila cheia, stop/restart,
  último pacote curto e ausência de alocações no callback.
- A fila IQ é forçada a encher antes de liberar o consumidor; os 97 quadros
  variáveis precisam chegar completos e em ordem, atravessando várias voltas.
- Os sinks reais são compilados com APIs nativas simuladas: RtAudio 5/6 e
  PortAudio, incluindo ausência de dispositivo, callbacks sem dados, erros
  de open/start/stop/close e erro assíncrono da API 6.
- Em 2,4 MHz, entradas com 333/8.192/65.536 amostras por pacote precisam
  produzir a mesma sequência dentro de erro relativo RMS `2e-5`, sem perda
  de frames e com cada publicação limitada a um hop.
- A integração gráfica dos onze modos passa pelo mesmo AudioBuffer em um
  dispositivo simulado com relógio de 10 ms, e testa taxas 48/44,1/96 kHz.
  O cenário NR recebe IQ completo de 2,4 MHz. Há 880 seleções normais e mais
  220 com NR, além de criação/remoção de rádios e checks dos controles salvos.
- A automação mantém os pacotes Linux existentes e os testes de abertura,
  reabertura, encerramento e isolamento da RTL.

Não há placa de som ou rádio físico conectado no ambiente de testes. Os
ensaios verificam o fluxo e os callbacks com dados sintéticos; não medem XRUN
de um driver específico nem garantem ausência de cortes quando a máquina
não consegue sustentar a taxa de entrada. A entrada IQ de 2,4 MHz continua
tendo o custo real de processar toda essa banda.
