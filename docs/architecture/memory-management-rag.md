# RAG: memória e alocação sem GC

Notas de recuperação para decisões de runtime e compilador. Este arquivo resume as fontes fornecidas para o projeto; não reproduz trechos extensos. As páginas indicadas são páginas impressas quando disponíveis e linhas aproximadas do extrator PDF/web.

## Pergunta de projeto

Como liberar memória de forma previsível sem tracing GC, limitando fragmentação e overhead tanto no compilador quanto no runtime da linguagem?

## Fontes e síntese

### Aho, Lam, Sethi e Ullman — *Compilers: Principles, Techniques, and Tools*, 2ª ed., capítulo 7

- A organização runtime separa áreas como código, dados estáticos, heap e stack; o compilador participa da representação e do acesso aos objetos junto com SO e hardware (p. 427–429, seção 7.1).
- Stack allocation depende de ativações de procedimentos aninhadas no tempo. Uma chamada empilha locais e o retorno recupera esse espaço, permitindo reutilizar o armazenamento de chamadas cujas vidas não se sobrepõem (p. 430, seção 7.2).
- O capítulo distingue gerenciamento de heap, referência/ownership e tracing. Reference counting é simples, mas imperfeito: contagem zero libera objetos, enquanto ciclos fortes não chegam a zero (seção 7.5.3; a discussão de objetivos em 7.5.1 trata desempenho, espaço/fragmentação e pausas).
- Recuperação para Snovac: locals devem ser liberados na saída do escopo pelo emitter/VM; valores heap precisam de move/drop/RC ou ownership exclusivo; o checker deve barrar ciclos fortes para que RC seja suficiente. A arena do AST é apropriada para a vida da compilação, mas não deve substituir a vida individual de objetos do usuário.

### Unicamp — *MC514: Gerenciamento de Memória*

- As notas percorrem o espaço stack/data/text, compactação, gerenciamento de blocos grandes e interfaces `malloc/free/realloc`, além de bitmap/lista de livres e políticas first-fit, next-fit, best-fit e worst-fit (slides/páginas impressas 17–22; a extração textual recupera esses títulos e diagramas).
- Recuperação para Snovac: não existe política de alocação que elimine todos os trade-offs; escolher arenas para dados com vida coletiva, pools para tamanhos/vidas repetidos e heap explícito para objetos independentes evita pagar buscas genéricas onde não são necessárias. Coalescing/reuso dependem de liberação correta e da política do allocator.

### Mohit Mishra — *Memory Allocation Algorithms*, Exploring OS

- A comparação didática descreve alocação de tamanho fixo como simples e previsível, porém com possível fragmentação interna; alocação variável atende tamanhos diversos, mas pode fragmentar externamente. Buddy divide potências de dois e facilita coalescing, com desperdício por arredondamento; slabs/pools favorecem objetos repetidos (seções 2–6).
- Qualidade/limite: fonte secundária explicativa, útil para vocabulário e trade-offs, não como especificação de allocator de produção. Os exemplos C são introdutórios e não trazem validação de overflow nem sincronização suficientes para copiar diretamente.
- Recuperação para Snovac: preferir mecanismos simples por classe de vida. Já temos arenas por compilação/execução, slabs de tarefas Pulsar e arrays com buffer geométrico que pode encolher; medir tamanhos e taxas antes de criar um allocator generalista, buddy allocator ou cache de objetos.

## Decisões resultantes para este repositório

1. **Compilação e estruturas temporárias:** `SnArena` por comando/unidade; libera em lote AST, tokens, símbolos e temporários após o consumidor terminar. O evaluator também usa arenas regionais com blocos iniciais de 4 KiB (incluindo scratch de interpolação), enquanto alocações grandes recebem blocos sob medida; isso reduz slack em regiões pequenas sem impor um pool fixo por chamada.
2. **Locais bytecode:** slots de frame são reutilizados e `OP_CLEAR_LOCAL` libera o valor owned na saída lexical, retorno ou salto de controle. Moves transferem a única referência owned; clones são explícitos.
3. **Objetos runtime:** strings/instâncias/variantes são co-alocados quando isso reduz chamadas e ponteiros; arrays mantêm header estável e buffer redimensionável. RC é atômico para valores owned compartilhados entre threads, mas mutação de slots exige sincronização própria.
4. **Ciclos:** containers rejeitam arestas fortes que criariam ciclo. `shared/weak` e destrutores gerais ainda precisam de especificação; não se deve prometer leak freedom apenas por usar RC.
5. **Dados de vida curta:** pools/slabs são aplicados quando o padrão já é claro, como tarefas de tamanho uniforme. O bloco de 64 tarefas reduz churn de alocação sem pré-alocar um pool grande e fixo por worker.
6. **Evaluator AST:** regiões privadas eliminam allocations de escopos escalares, buffers de arrays, strings temporárias e instâncias que não escapam ao sair. Arrays, strings, objetos, variantes, lambdas e ambientes registram suas regiões proprietárias. Retornos/stores percorrem iterativamente o grafo alcançável (incluindo ambientes capturados); apenas as regiões donas desses valores são preservadas, com fallback que retém todas as regiões se a travessia falhar. Regiões escapáveis continuam retidas até o fim da execução: esta promoção reduz retenção de ancestrais, mas ainda não substitui drops/ownership individuais no evaluator AST.
7. **Observabilidade:** `SNOVA_MEMORY_STATS=1` separa bytes usados, reservados, pico e contagens de valores quando disponíveis. Medir retenção/slack e workloads reais antes de trocar malloc/realloc por allocator próprio.

## Lacunas para encerrar a direção sem GC

- Definir ownership por tipo e ABI para todos os caminhos (AST, bytecode, nativo e FFI), incluindo substituição, retorno, erro, `defer` e encerramento de recursos.
- Concluir move/borrow checking em CFG, rejeitar aliases mutáveis incompatíveis e definir `shared/weak` para grafos que precisem de ciclos.
- Levar drops determinísticos para o evaluator AST ou removê-lo do caminho de produção; regiões conservadoras ainda retêm dados escapáveis vivos até o fim da execução.
- Cobrir os contratos com validação de runtime/benchmark: sem use-after-free/double-drop, contadores vivos zerados no teardown, crescimento limitado em loops e custo de alocação/slack comparado ao baseline.

## Referências

- Aho et al., 2007, capítulo 7: [PDF](https://dpvipracollege.ac.in/wp-content/uploads/2023/01/Alfred-V.-Aho-Monica-S.-Lam-Ravi-Sethi-Jeffrey-D.-Ullman-Compilers-Principles-Techniques-and-Tools-Pearson_Addison-Wesley-2007.pdf), seções 7.1–7.2 e 7.5.1–7.5.3.
- Unicamp, MC514, *Gerenciamento de Memória*: [PDF](https://www.ic.unicamp.br/~islene/mc514/memoria/memoria.pdf), slides/páginas impressas 17–22.
- Mohit Mishra, *Day 18: Memory Allocation Algorithms*: [artigo](https://mohitmishra786.github.io/exploring-os/src/day-18-memory-allocation-algorithms.html), seções 2–6.
