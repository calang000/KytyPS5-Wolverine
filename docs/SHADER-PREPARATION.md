# Avaliação: preparação de shaders antes do jogo

A sugestão de uma etapa de pré-compilação é pertinente para travadas de
compilação. Ela não demonstra, por si, ganho no FPS sustentado.

## O que o código já faz

- `pipelineCache.cpp::InitializeDriverCache` valida a assinatura do cache Vulkan
  contra dispositivo, driver e demais dados de compatibilidade antes de carregá-lo.
- `ProgramCache::DiskKey` e `programDiskCache.cpp` persistem programas traduzidos
  e planos, com chaves de shader, especialização e versão do backend.
- `FindWarmStage` e os caminhos de compilação tentam reaproveitar entradas já
  carregadas. Os shaders e as permutações são descobertos durante a execução.
- Existe checkpoint do cache do driver e gravação no encerramento normal.

## Limitação da pré-compilação completa

Dispor de shaders traduzidos não basta para reconstruir todo pipeline: também
são necessários estados de desenho, recursos, especializações, formatos de
attachments, opções de compilação e capacidades do dispositivo. O cache atual
não constitui um catálogo completo, independente do jogo, de todas essas receitas.
Não basta percorrer o dump para prometer que toda a campanha está pré-compilada.

Uma implementação futura viável começaria registrando receitas de pipelines
já observadas, com versões e validação de compatibilidade, e reconstruindo essas
receitas no próximo lançamento. A etapa deve ser cancelável, indicar o que foi
preparado, não exigir travessia automática da campanha e manter fallback normal
quando um shader ou estado ainda for desconhecido. Isso exige trabalho de
persistência e testes, não apenas uma barra de progresso.

## Evidência disponível e decisão atual

Nas capturas já coletadas, houve reutilização de cache e relatórios com zero
misses de programas enquanto o desempenho ainda era baixo. Ao mesmo tempo,
existem esperas CPU/GPU e custo de materialização/gravação de comandos. Portanto,
pré-compilação e FPS sustentado devem ser avaliados separadamente.

Decisão: preservar caches compatíveis no launcher e documentar a primeira
execução; manter o catálogo de receitas em avaliação. Não foi implementada
uma falsa etapa de "100% compilado" e não são distribuídos caches do jogo.

Esta análise se refere à candidata derivada de `aa0cde67`, não a uma avaliação
de recursos de versões futuras do KytyPS5 ou de outros emuladores.