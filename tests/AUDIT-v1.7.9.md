# SDRPP-FULL v1.7.9 — Noise Reduction e ciclo de vida

Base: `5b5cc5cb0efe4df0592d016f96964a87223a69b6` (v1.7.8).
Revisão focada no consumo dos três caminhos de redução de ruído, na troca
dos onze modos e na inicialização/remoção dos processadores de NR.
Bandplans, colormaps, temas, ícones e créditos foram preservados.

## Correções

- **IF NR dos modos:** executava uma FFT inversa completa para cada amostra,
  embora o espectro tivesse apenas um bin não nulo e somente uma amostra da
  saída fosse aproveitada. A identidade da DFT permite calcular essa mesma
  amostra com uma multiplicação por um fasor constante. A FFT de análise, a
  janela Nuttall, a escolha do bin, os quatro presets e o ganho permanecem.
  Dois buffers e o plano da FFT inversa deixam de ser necessários.
- **Baseband NR / LogMMSE:** o processamento já utilizava a taxa após a
  decimação; não havia uma sobreamostragem artificial a remover. Foram
  eliminadas cópias de janelas e vetores temporários, reutilizando o espaço
  de trabalho e fundindo a aritmética do estimador. FFT, sobreposição, histórico,
  largura recebida, limites e tabelas E1/expfast mantêm seus parâmetros.
  Chamadas sem uma nova janela completa já não recalculam/adaptam o modelo
  nem enviam pacotes vazios. Um piso de potência de `1e-20` evita `0/0` quando
  a entrada é exatamente silenciosa.
- **Audio NR / OMLSA-MCRA:** antes processava apenas um bloco por chamada;
  com pacotes maiores, a fila crescia continuamente. Agora drena os blocos
  disponíveis, limita cada saída ao tamanho do stream e escoa um restante
  completo antes de aceitar outro pacote. Os buffers PCM são reutilizados e
  a fila é compactada uma vez por pacote. A escala é ajustada antes da
  conversão para 16 bits, com proteção contra valores fora da faixa.
- **Memória do Audio NR:** os destrutores usavam `if (!ponteiro)` para liberar
  arrays válidos e omitiam duas alocações. Os proprietários são inicializados
  e todos os arrays são liberados. A tabela de 700.000 ganhos inteiros passa
  a ser uma única tabela imutável por módulo, compartilhada entre rádios e
  reinicializações. Os valores e a resolução da tabela são os mesmos.
- **Limites do denoiser:** o espelhamento do bin DC gravava no índice N de
  arrays de N elementos e podia sobrescrever o próximo FFT de áudio empacotado.
  DC agora permanece no seu próprio índice. O contador de quadros deixa de
  usar `short`, evitando seu retorno ao negativo após 32.767 quadros.
- **Iniciar/parar:** falha de `swap` encerra o trabalhador do Audio NR em vez
  de continuar o laço. Destrutores param trabalhadores antes de liberar o
  estado. Reset, controles de NR e dados publicados para a interface são
  sincronizados; o perfil é marcado para reset antes da criação da thread.
  A assinatura do evento TX acompanha start/stop, inclusive paradas temporárias.
- **Inicialização:** a ligação do NR ao frontend e aos rádios ocorre em
  `postInit`, após a criação das instâncias configuradas. Módulos desativados
  não são ligados temporariamente durante o construtor. A remoção de um rádio
  desliga seu Audio NR antes de destruir a interface; nomes reutilizados e
  ativações repetidas não deixam processadores antigos na cadeia. Não foram
  acrescentadas esperas fixas nem atrasos de carregamento.

## Medições locais

Ubuntu Noble, GCC 13, `-O3`, mesmo ambiente e entrada determinística;
mediana de três execuções intercaladas antes/depois. São tempos de CPU do
processamento testado, não a ocupação total do aplicativo ou uma previsão
para o computador do usuário.

| IF NR, 262.144 amostras | Antes (ms) | Depois (ms) | Redução |
|---|---:|---:|---:|
| 9 bins | 20,447 | 16,269 | 20,4% |
| 15 bins | 23,534 | 17,495 | 25,7% |
| 31 bins | 74,129 | 42,885 | 42,1% |
| 32 bins | 30,477 | 20,133 | 33,9% |

| Baseband NR | CPU antes → depois, ms por segundo de sinal | Alocações C++ antes → depois, por segundo |
|---|---:|---:|
| 192 kHz | 10,470 → 10,865 | 2.754 → 330 |
| 1 MHz | 81,353 → 78,310 | 3.201 → 708 |
| 2,4 MHz | 289,891 → 189,403 | 3.892 → 710 |

O ganho mais relevante foi **34,7% menos CPU no Baseband NR a 2,4 MHz**.
A 192 kHz a diferença foi de +0,395 ms por segundo, cerca de 0,04 ponto
percentual de um núcleo. Não houve corte de taxa, banda, bins ou recursos
para obter os ganhos. Baseband NR de banda larga continua tendo custo
proporcional à taxa de entrada e ao seu histórico espectral.

## Verificação reproduzível

```sh
bash scripts/test_noise_reduction.sh build memory
CXX=clang++ bash scripts/test_noise_reduction.sh build status
bash scripts/test_noise_reduction.sh build benchmark
bash scripts/test_radio_gui_modes.sh build nr
```

- ASan: IF NR comparado a um oráculo com FFT inversa completa nos quatro
  presets e em pacotes pequenos/irregulares; LogMMSE comparado às expressões
  vetoriais originais em 8/44,1/192 kHz, com sinal e silêncio. Tolerâncias de
  erro relativo RMS: `2e-6` e `2e-5`, respectivamente.
- Audio NR: saída idêntica amostra a amostra ao backend em blocos completos
  e pacotes irregulares em 8/44,1/48/96 kHz; pacotes do tamanho máximo do
  stream, troca de taxa, entrada acima da escala, proteção do FFT vizinho,
  passagem por 32.767 quadros e término em stream parado.
- 160 reinicializações do denoiser: nenhuma alocação C++ proprietária restante
  após liberar cada instância, descontada a tabela compartilhada já aquecida.
- TSan: controles/status/reset concorrentes, pacotes incompletos, start/stop
  repetidos e alterações de taxa do Audio NR.
- A integração gráfica com NR cobre os 110 pares dirigidos entre NFM, WFM,
  AM, DSB, RAW, USB, LSB, CW, CW-R, DSD e oldDSD; alterna 220 vezes,
  modifica taxas de áudio, ativa/desativa NR e cria/remove rádios com nomes
  reutilizados. Complementa a regressão gráfica normal de 880 seleções.
- A automação inclui esses testes no Noble, valida abertura/reabertura do
  pacote e constrói os pacotes existentes para Debian Bookworm/Trixie,
  Ubuntu Jammy/Noble/Resolute/Stonking e AppImage. A biblioteca RTL privada
  e as verificações de isolamento do sistema foram mantidas.

Os testes usam sinais sintéticos. Não substituem a medição no computador
do usuário, nem validam recepção real de todos os protocolos DSD ou qualquer
combinação de kernel, driver, rádio e dispositivo de áudio.
