# Publicação de releases

As releases são mantidas em `calang000/KytyPS5-Wolverine`. Uma release deste fork
não é uma release oficial do projeto KytyPS5 upstream.

1. Desenvolver em uma branch de pesquisa, com alterações reversíveis.
2. Compilar em Release e executar os testes correspondentes às alterações.
3. Para desempenho, medir a mesma cena de save, configuração e hardware,
   descartar controles instáveis e validar movimento, carregamento, saves e saída.
4. Só incorporar alterações que tragam correção ou ganho demonstrado; opções
   reprovadas permanecem desligadas ou fora da branch de publicação.
5. Registrar a validação em `.github/release-validation/<tag>.json`. A fonte
   validada deve ter o mesmo conteúdo de `src/`, `CMakeLists.txt` e `3rdparty/`
   do commit publicado. Não publicar logs pessoais ou arquivos do jogo.
6. Criar uma tag `vX.Y.Z-rc.N` para pré-releases experimentais. Uma tag estável
   `vX.Y.Z` exige validação estável explícita, incluindo encerramento normal.
7. O workflow Windows compila, testa, empacota e publica somente tags com esse
   registro. Commits comuns e branches de pesquisa não geram releases públicas.

O pacote inclui código do emulador, launcher, licença e instruções. Dumps,
firmware, chaves, saves e caches de shaders do jogo não são distribuídos.

Para compilar localmente, veja o README. Para empacotar uma build compilada:

```powershell
.\tools\wolverine\Package-Release.ps1 -BuildDirectory _Build\windows -OutputDirectory dist -Version v0.1.0-rc.1
```

Mudar o compilador/recompilador, ABI de shaders ou especializações exige revisar
as versões e assinaturas dos caches; não forçar a reutilização de caches antigos
para esconder uma incompatibilidade.
