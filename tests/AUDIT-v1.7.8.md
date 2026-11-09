# SDRPP-FULL v1.7.8 — revisão de estabilidade

Base examinada: `ac60be92cda5c5d37ea09ba68c30da089e03fefb`, incluindo as quatro
alterações de nomes de recursos feitas por João após a v1.7.7. Os arquivos de
bandplans, colormaps, temas, ícones e os créditos foram preservados.

## Correções

- **Reamostragem:** a divisão preliminar por potência de dois pode produzir
  uma taxa fracionária. Arredondá-la antes de calcular a razão racional gerava
  bancos enormes e um pequeno desvio na quantidade de amostras. Para taxas
  inteiras em Hz, a nova rotina reduz a razão original exatamente quando isso
  não aumenta o banco. Clocks fracionários/incomuns mantêm a aproximação
  anterior. O limite de pré-decimação agora usa o expoente correto.
- **AGC:** os controles e o processamento compartilhavam estado sem uma
  sincronização comum. ThreadSanitizer reproduziu uma corrida entre `getGain`
  e `process`. Há agora uma trava por bloco, separada da trava usada para
  iniciar/parar trabalhadores; a interface lê um ganho publicado atomicamente.
  Ataque, recuperação, ponto de operação, limitador e ganho manual mantêm os
  mesmos cálculos. A alteração de tom CW/CW-R também é sincronizada com o
  rotador, preservando sua fase entre blocos.
- **Configuração:** os retornos por caminho vazio ou diretório deixavam um
  mutex preso. O carregamento usa RAII, recupera objetos JSON inválidos e o
  autosave usa estado sincronizado. No Linux, o arquivo novo é escrito em um
  temporário privado no mesmo diretório e renomeado sobre o anterior apenas
  após terminar a escrita. Um encerramento durante a escrita não trunca a
  configuração anterior. Links simbólicos existentes continuam apontando ao
  destino original. Isto não promete persistência contra queda de energia.
  O encerramento também interrompe a espera do autosave quando o config está
  ocupado, em vez de esperar uma pausa fixa de um segundo por módulo.
- **Fechamento:** a aplicação para a fonte ativa, encerra o frontend e destrói
  as instâncias antes de chamar `_END_` nas bibliotecas e fechar a interface.
  Antes, o caminho normal chamava `_END_` sem destruir as instâncias. Uma
  fábrica de módulos que devolva ponteiro nulo agora é rejeitada com erro.
- **Recursos:** JSON/cores inválidos em tema ou colormap são rejeitados com
  diagnóstico. A falta do tema Dark/Turbo já não causa acesso a entrada
  inexistente; só nessa situação há um fallback. Arrays RGB têm um proprietário
  explícito, inclusive ao substituir mapas com o mesmo nome. Os recursos válidos
  do usuário não são alterados. A tela de abertura passa a informar separadamente
  a inicialização da fonte e do áudio após os colormaps.

## Medições locais comparáveis

Ubuntu Noble, GCC 13, otimização `-O2`, mesmo ambiente e mesma entrada sintética;
v1.7.7 comparada à revisão. Tempos ilustrativos de uma execução, sujeitos ao
escalonador e à CPU. Não representam ganho percentual do aplicativo inteiro.

| Conversão | Coeficientes antes → depois | Configuração antes → depois |
|---|---:|---:|
| 2.400.000 → 3.000 Hz | 44.531 → 1.900 | 2.439 → 196 µs |
| 10.000.000 → 15.000 Hz | 1.484.375 → 9.500 | 78.304 → 528 µs |
| 250.000 → 5.000 Hz | 593.750 → 1.900 | 32.100 → 119 µs |

Dez segundos na primeira conversão passaram de **29.997 para 30.000 amostras**,
o valor esperado. A rejeição dos tons fora da faixa ficou, respectivamente,
em aproximadamente **−117,9 / −131,0 / −119,6 dB** antes e depois. O ganho medido
no teste variou menos de 0,001 em valor absoluto. As respostas originais dos
filtros de pré-decimação, inclusive sua pequena ondulação, foram mantidas.

AGC, 25,6 milhões de amostras: real 50,3 → 50,0 ms; complexo 90,3 → 92,6 ms.
A diferença é pequena nesta medição; não é uma promessa de desempenho.
Os testes verificam ponto de operação em sinal fraco/forte, ataque, recuperação,
ganho manual, limite e silêncio. Não houve mudança da curva do AGC para obter
essa sincronização.

## Regressões e validação automatizada

```sh
bash scripts/test_runtime_audit.sh build memory
CXX=clang++ bash scripts/test_runtime_audit.sh build status
bash scripts/test_dsd_modes.sh build memory
CXX=clang++ bash scripts/test_dsd_modes.sh build status
bash scripts/test_radio_gui_modes.sh build
```

- ASan/UBSan cobrem as rotinas instrumentadas de reamostragem, AGC,
  configuração/recursos e os testes de DSD/modos já existentes.
- TSan verifica controles/telemetria do AGC real e complexo, tom CW,
  autosave e os testes de status DSD/oldDSD da v1.7.7.
- Os testes de DSP fazem 440 transições: todos os 110 pares dirigidos duas vezes
  entre NFM, WFM, AM, DSB, RAW, LSB, USB, CW, CW-R, DSD e oldDSD.
- O teste GUI usa o Radio/VFO/frontend reais e IQ sintético, muda o dispositivo
  simulado entre 48 / 44,1 / 96 kHz e verifica áudio finito, bandwidth válido e
  preservação dos ajustes de tom CW/CW-R e HPF FM/AM. Mede latência de troca,
  CPU do processo e RSS após aquecimento/final. Deixa a recepção ativa ao fechar
  para exigir o encerramento correto do programa, sem o próprio teste parar
  previamente os trabalhadores.
- No contêiner descartável Noble do CI, `test_package_startup.sh` extrai o .deb
  e testa primeira abertura com os caminhos instalados e os módulos padrão,
  reabertura com a configuração salva e recuperação de temas/mapas inválidos.
  Um observador temporário verifica que as instâncias foram destruídas antes
  do desligamento dos módulos. Ele nunca entra no pacote distribuído.
- A matriz Linux existente e a verificação da librtlsdr privada 2.0.3 permanecem.
  A release única `Realease`, as notas de João e os sufixos de distribuição dos
  downloads são preservados.

Os testes locais não dispõem de receptor físico nem dispositivo de áudio real.
A interface completa é executada no CI com Xvfb/Mesa. ASan não instrumenta as
bibliotecas externas e todos os decodificadores vinculados; LeakSanitizer não
é usado neste ambiente. Não se afirma compatibilidade com todo kernel,
distribuição, driver ou hardware existente, nem ausência absoluta de bugs.
O travamento histórico que ficou mostrando “touch.json” não teve backtrace
capturado: estas correções não comprovam a causa daquele episódio específico.
