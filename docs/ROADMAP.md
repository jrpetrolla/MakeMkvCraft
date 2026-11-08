# Roadmap técnico do MakeMkvCraft

Atualizado em: 2026-11-07

Este documento é o backlog técnico oficial para aproximar o MakeMkvCraft da
robustez esperada de um remuxer de DVD/Blu-ray, sem copiar código proprietário.
A ordem prioriza correção do core, evidência reproduzível e comportamento
fail-closed antes de ampliar a interface.

## Convenções

- `[x]`: implementação concluída e verificação local executada.
- `[ ]`: pendente.
- Um item que depende de mídia real só pode ser concluído após remux, fechamento
  do trailer e auditoria do MKV resultante.
- Detectar uma mídia não significa suportá-la. Suporte exige selecionar um
  título lógico, lê-lo pelo modelo de navegação correto, remuxar, finalizar e
  verificar a saída.
- ISOs, estruturas copiadas, VOB/M2TS, logs sensíveis e fixtures proprietárias
  permanecem fora do Git.

## Sequência de execução

1. Validar o caminho atual de DVD ISO.
2. Tipar o modelo de domínio e implementar as flags MKV.
3. Implementar seleção real da unidade óptica.
4. Unificar DVD ISO, pasta e unidade no pipeline `libdvdnav`.
5. Criar regressões automatizadas e modularizar o core.
6. Tornar Blu-ray realmente funcional por playlist.
7. Ampliar robustez, distribuição e paridade funcional.

---

## P0 — Validar o estado atual

1. [ ] Testar o build atual com `DISNEY_FABLES_V.ISO`.
2. [ ] Testar o build atual com `NET_GAMES.ISO`.
3. [ ] Confirmar término do remux, trailer, duração, sincronia, reprodução até o
   fim, idiomas, capítulos, legendas e ausência de regressão DTS.
4. [ ] Auditar os MKVs com `ffprobe` e `mkvmerge -J`.
5. [ ] Registrar relatório de regressão sem versionar mídia ou logs sensíveis.

### Gate P0

- Os dois títulos terminam com sucesso.
- O MKV abre, possui a duração esperada e toca até o EOF.
- Nenhuma regressão DTS permanece no log.

## P1 — Estruturar o modelo do core

6. [x] Substituir identificação textual por `SourceKind` (`IsoImage`, `Folder`,
   `OpticalDrive`) e `DiscKind` (`DvdVideo`, `BluRay`).
7. [x] Preservar identificadores físicos: DVD (VTS, TTN, PGC e ângulo) e
   Blu-ray (playlist, clipes e PIDs).
8. [x] Separar metadados detectados da configuração de saída.
9. [x] Adicionar à configuração de faixa: seleção, `mkv_default`, `mkv_forced`
   e indicação forced detectada na origem.
10. [x] Remover despacho do core baseado em textos como `DVD-Video (ISO)`.

## P1 — Tags MKV `Default` e `Forced`

11. [x] Adicionar colunas **Default** e **Forced** à árvore.
12. [x] Permitir `Default` para áudio e legenda.
13. [x] Manter vídeo único como default automaticamente.
14. [x] Permitir `Forced` somente para legenda.
15. [x] Limitar a uma faixa default por tipo em cada título.
16. [x] Permitir que `Default` e `Forced` coexistam.
17. [x] Marcar uma flag deve selecionar a faixa.
18. [x] Desmarcar a faixa deve limpar suas flags.
19. [x] Inicializar `Forced` quando a origem DVD indicar `code_extension == 9`,
   permitindo alteração manual.
20. [x] Aplicar explicitamente `AV_DISPOSITION_DEFAULT` e
   `AV_DISPOSITION_FORCED`.
21. [x] Registrar as disposições aplicadas no log.
22. [ ] Validar as disposições com `ffprobe` e `mkvmerge -J`.

### Gate das flags MKV

- Há no máximo uma faixa default por tipo e título.
- Forced só aparece em legendas.
- Faixa não selecionada não mantém disposição de saída.
- O MKV final confirma as disposições escolhidas pelo usuário.

## P1 — Seleção da unidade óptica

23. [x] Enumerar unidades Windows com `GetLogicalDriveStringsW` e
   `GetDriveTypeW == DRIVE_CDROM`.
24. [x] Obter letra, rótulo e estado da mídia.
25. [x] Substituir o seletor genérico de pasta por uma lista de unidades ópticas.
26. [x] Adicionar atualização da lista de unidades.
27. [x] Tratar nenhuma unidade, bandeja vazia, mídia ilegível, remoção durante a
   análise e acesso negado.
28. [x] Preservar `SourceKind::OpticalDrive` durante inspeção e remux.
29. [x] Não reintroduzir o botão **Ejetar**.

### Gate da unidade óptica

- O seletor mostra apenas unidades ópticas reais.
- Unidade vazia não é enviada ao inspector.
- O tipo de origem continua `OpticalDrive` até o remux.

## P1 — Corrigir DVD em pasta e unidade física

30. [x] Usar o mesmo pipeline `libdvdnav` para DVD ISO, pasta `VIDEO_TS` e
   unidade óptica.
31. [x] Eliminar a associação incorreta entre título N e `VTS_NN_1.VOB`.
32. [x] Ler o título lógico completo, com todos os VOBs e células relacionados.
33. [x] Resolver corretamente VTS, TTN, PGC e capítulos.
34. [x] Calcular duração pelo PGC correto, não apenas pelo primeiro.
35. [x] Preservar e validar múltiplos ângulos.
36. [x] Manter ângulo 1 como padrão até existir seletor de ângulo.
37. [x] Tratar eventos `WAIT` e `STILL`.
38. [x] Manter recuperação limitada de setores e comportamento fail-closed.
39. [ ] Validar DVD ISO, pasta e disco físico com o mesmo conjunto de testes.
40. [ ] Definir e documentar a política de DVDs criptografados e `libdvdcss`.

### Gate do pipeline DVD

- A mesma identidade lógica produz o mesmo conjunto de faixas nas três origens.
- Nenhum caminho presume nome de VOB a partir do ordinal do título.
- Falhas deixam log suficiente e removem saída parcial e temporários.

## P1 — Testes automatizados do core

41. [ ] Introduzir CTest.
42. [ ] Criar fixtures para reinício real de SCR, excursão falsa, reset nos
   últimos pacotes, regressões de 2 ms e 28 ms, streams esparsos no EOF,
   pacotes sem posição, limite do buffer e regressão/igualdade de DTS.
43. [ ] Testar seleção e remoção individual de áudio, legenda e capítulos.
44. [ ] Testar flags `Default` e `Forced`.
45. [ ] Testar VobSub fragmentado e paleta.
46. [ ] Testar progresso monotônico.
47. [ ] Testar remoção de MKV e VOB temporários após falha injetada.
48. [ ] Manter testes com ISOs reais separados das fixtures sintéticas.

## P2 — Modularizar o core

49. [ ] Dividir `MkvRemuxer.cpp` em navegação DVD, navegação Blu-ray, extração,
   reparo de timestamps, mapeamento de faixas, mux Matroska e diagnóstico.
50. [ ] Criar biblioteca de core independente da interface Qt.
51. [ ] Criar CLI mínima para inspeção e regressões automatizadas.
52. [ ] Remover ou encapsular `avpriv_stream_set_need_parsing`.
53. [ ] Preservar o caminho comprovado de DVD ISO durante a refatoração.
54. [ ] Manter o VOB temporário até existir alternativa direta comprovadamente
   estável.

## P2 — Corrigir Blu-ray

55. [ ] Mapear corretamente os enums de codecs do `libbluray`.
56. [ ] Corrigir resolução, canais, idioma e tipo das faixas.
57. [ ] Deduplicar faixas repetidas entre clipes.
58. [ ] Preservar playlist, clipes e PIDs reais.
59. [ ] Selecionar o título pela playlist.
60. [ ] Ler por `bd_select_playlist()` e `bd_read()`.
61. [ ] Eliminar ISO direto no FFmpeg, associação título → `00001.m2ts` e
   presunção de clipe único.
62. [ ] Suportar seamless branching e playlists com múltiplos clipes.
63. [ ] Gravar capítulos da playlist.
64. [ ] Aplicar seleção, idioma, `Default` e `Forced` às faixas Blu-ray.
65. [ ] Implementar o mesmo pipeline para ISO, pasta e unidade.
66. [ ] Detectar AACS/BD+ e apresentar erro claro quando não suportado.
67. [ ] Só declarar Blu-ray funcional após remux completo e verificado.

### Gate Blu-ray

- A playlist real é selecionada e lida pela API do `libbluray`.
- Seamless branching mantém duração, capítulos e continuidade.
- Codecs e metadados conferem com `ffprobe`/`mkvmerge -J`.

## P2 — Robustez e experiência de uso

68. [ ] Verificar espaço livre antes de iniciar.
69. [ ] Tratar saída existente: substituir, renomear ou cancelar antes do remux.
70. [ ] Diferenciar erros de inspeção, leitura, timestamp e mux.
71. [ ] Validar o MKV após fechar o trailer.
72. [ ] Preservar seleção do usuário ao atualizar a árvore.
73. [ ] Adicionar seleção de ângulo para DVDs multiângulo.
74. [ ] Adicionar filtro opcional de duração mínima.
75. [ ] Persistir apenas preferências úteis, como última pasta e idioma.

## P2 — Distribuição e documentação

76. [ ] Manter somente `dist/windows-x64`.
77. [ ] Informar em cada build quais arquivos precisam ser substituídos.
78. [ ] Normalmente atualizar apenas `MakeMkvCraft.exe`.
79. [ ] Auditar arquitetura e dependências de todos os binários.
80. [ ] Gerar SHA-256 do executável.
81. [ ] Resolver a divergência entre documentação e presença de
   `libdvdcss-2.dll` na distribuição.
82. [ ] Documentar licenças e componentes de terceiros.
83. [ ] Atualizar matriz de suporte para DVD/Blu-ray em ISO, pasta, unidade e
   mídia criptografada.
84. [ ] Não gerar instalador ou ZIP durante a fase de testes.

## P3 — Paridade futura com a referência

85. [ ] Regras automáticas de seleção por idioma e tipo.
86. [ ] Perfis de seleção e saída.
87. [ ] Conversões opcionais, como LPCM → FLAC.
88. [ ] Closed captions.
89. [ ] Remux direto sem VOB temporário, apenas após prova de estabilidade.
90. [ ] Backup de disco.
91. [ ] Servidor de streaming.
92. [ ] Suporte AACS/BD+ após avaliação técnica e jurídica.
93. [ ] Avaliar UHD Blu-ray.
94. [ ] Manter HD-DVD fora do escopo até decisão explícita.

## Registro de execução

| Data | Pacote | Evidência | Estado |
|---|---|---|---|
| 2026-11-07 | Modelo tipado e flags MKV (6–21) | Build Win64 MXE concluído; validação de MKV real pendente | Implementado |
| 2026-11-07 | Unidade óptica (23–29) | Build Win64 MXE concluído; teste em hardware pendente | Implementado |
| 2026-11-07 | Pipeline DVD unificado (30–38) | Build Win64 MXE concluído; regressão ISO/pasta/unidade pendente | Implementado |
