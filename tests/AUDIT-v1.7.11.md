# SDRPP-FULL v1.7.11 — resposta dos controles RTL

Base: `a56b21094c7435cd1dcdbf836f80d5656c20ccbd` (v1.7.10).
Correção motivada pelo atraso no ganho, RTL AGC e troca de modos depois de
algum tempo de recepção. O LogMMSE passou a consumir menos CPU na revisão
anterior e seus cálculos não foram alterados nesta revisão.

## Causa e correção

O callback de recepção do RTL fazia conversão IQ e `stream.swap()` dentro
do atendimento de eventos USB. Essa publicação espera quando o consumidor
DSP está ocupado. Com a fila IQ agora preservando os dados e a saída sendo
cadenciada pela placa de som, essa espera podia atingir a própria recepção
USB. Comandos síncronos de ganho/AGC/sintonia, chamados pela interface,
precisam do mesmo contexto de eventos do libusb e ficavam esperando.

O callback passa a copiar os bytes recebidos para uma fila SPSC pré-alocada.
Um trabalhador separado mantém a conversão IQ original e entrega os pacotes
ao mesmo stream. Somente esse trabalhador pode esperar pelo DSP; o callback
USB não espera por espaço, não adquire mutex DSP e não aloca memória.

A fila comporta 32 pacotes, aproximadamente 80 ms com o tamanho de transferência
atual. Em 2,4 MHz, são 376.832 bytes de armazenamento IQ bruto. Ela não cresce
com o tempo. Se o processamento ficar realmente saturado e a fila encher,
o pacote novo é recusado e contabilizado; dados ainda sendo lidos nunca são
sobrescritos. A quantidade é registrada ao parar a recepção. Isso permite
continuar atendendo comandos e encerramento mesmo sob sobrecarga; não promete
recepção sem perdas quando a máquina não acompanha a entrada em tempo real.

Start inicializa o armazenamento antes de iniciar USB. Stop interrompe e junta
o trabalhador DSP, cancela/junta a recepção USB e só então fecha o dispositivo.
Pacotes pendentes não são reproduzidos ao reiniciar. A biblioteca RTL privada
2.0.3, o suporte V4L e os patches de encerramento USB permanecem intactos.

## Verificações

```sh
bash scripts/test_audio_flow.sh build memory
CXX=clang++ bash scripts/test_audio_flow.sh build status
bash scripts/test_noise_reduction.sh build memory
bash scripts/test_radio_gui_modes.sh build
bash scripts/test_radio_gui_modes.sh build nr
```

- Um ensaio reproduz a publicação bloqueante antiga dentro do contexto USB:
  o comando permanece bloqueado pelos 250 ms observados, até liberar o DSP.
  Com a fila de produção cheia e o consumidor parado, o comando volta a
  concluir; são exigidos menos de 100 ms, inclusive para stop.
- Localmente, o comando simulado com a fila nova levou 0,105 ms em ASan/UBSan
  e 1,920 ms em TSan. São medições deste ambiente, sem rádio físico.
- O módulo RTL real é compilado com uma API de dispositivo simulado. Executa
  256 sequências de ganho/AGC/sintonia com DSP parado e oito start/stop completos.
  O maior tempo local das sequências foi 0,085 ms; nenhum dispositivo ou thread
  ficou aberto e nenhum pacote antigo restou publicado após stop.
- A fila é verificada com 768 pacotes variáveis, conversão amostra a amostra
  idêntica à anterior, múltiplas voltas, nenhuma perda no caminho normal e
  120 paradas/reinícios concorrentes. ASan/UBSan e TSan cobrem esses fluxos.
- Os testes existentes de áudio/IQ continuam cobrindo fila cheia, callbacks
  variáveis, mono/estéreo, 44,1/48/96 kHz e os sinks RtAudio 5/6 e PortAudio.
- Os oráculos numéricos do NR continuam passando nos quatro presets,
  estimador/janelas/ganhos, tamanhos variáveis e entrada completa de 2,4 MHz.
- Na integração gráfica, o cenário NR usa a nova entrada RTL com relógio
  simulado de 2,4 MHz e comandos concorrentes com os eventos USB. Verifica
  220 seleções nos onze modos e impõe limite de 100 ms aos comandos simulados.
  O cenário normal mantém 880 seleções; ambos usam a saída de áudio real
  do aplicativo com uma placa simulada e alterações de taxa de áudio.

A matriz de CI também executa abertura/reabertura/encerramento dos pacotes e
a verificação do isolamento da RTL. Não há rádio ou placa de som física
conectados neste ambiente; a confirmação no hardware do usuário complementa
os ensaios sintéticos. Não foram alterados filtros, AGC dos demoduladores,
protocolos DSD/oldDSD, recursos visuais, bandplans ou colormaps.
