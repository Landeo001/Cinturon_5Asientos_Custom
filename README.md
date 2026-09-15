DOCUMENTO TÉCNICO
Lógica y Comportamiento del Sistema de Monitoreo de Cinturones de Seguridad
Especificación funcional del sistema

1. Objetivo del sistema
El Sistema de Monitoreo de Cinturones de Seguridad tiene como objetivo controlar el estado de los cinturones de seguridad de los ocupantes de un vehículo y condicionar la habilitación del vehículo al cumplimiento de las condiciones de seguridad configuradas. El sistema combina entradas digitales, detección de movimiento, indicadores LED, reproducción de mensajes de audio y un relé de bloqueo.

2. Configuración y modos de operación
La lógica de operación se determina mediante la cantidad de asientos configurados en la memoria NVS, identificada mediante el parámetro ca. Existen dos modos principales.

Característica	Mono-asiento (1)	Multi-asiento (2 a 5)
Audio al dar contacto	Audio de bienvenida	Audio de Bienvenida
Pulsador de pasajeros	Deshabilitado	Habilitado; selecciona de 1 a 5 pasajeros
Indicadores LED	No se usan los LEDs	Verde = abrochado; rojo = alerta; apagado = inactivo
Habilitación de vehículo	Piloto abrochado	Piloto abrochado + cinturones abrochados según lo indicado
3. Lógica general de funcionamiento
El funcionamiento se organiza como un ciclo de viaje compuesto por cuatro fases: encendido, declaración y verificación, desbloqueo y apagado. El sistema mantiene el relé activo mientras exista una condición que impida habilitar la circulación.

3.1. Fase 1 - Encendido (Contacto ON)
El sistema detecta un nivel alto en PIN_CONTACTO, correspondiente a la presencia de 12 V/24 V.

Se activa PIN_RELAY = 1, manteniendo bloqueado el vehículo.

Se eliminan las infracciones correspondientes al ciclo de viaje anterior.

Se reproduce el audio de inicio: Audio 15 para configuración de un asiento y Audio 11 para configuración de 2 a 5 asientos.

En configuración multi-asiento, el sistema queda a la espera de la declaración del número de ocupantes mediante el pulsador.

3.2. Fase 2 - Declaración y verificación
Configuración de un asiento:

El sistema asume automáticamente un ocupante. No requiere interacción con el pulsador y el desbloqueo depende exclusivamente de que el piloto tenga el cinturón abrochado.

Configuración de 2 a 5 asientos:

(El documento original presenta aquí caracteres corruptos que impiden recuperar el contenido de esta sección.)

3.3. Fase 3 - Verificación de seguridad y desbloqueo
El relé solo puede pasar a estado de reposo (PIN_RELAY = 0) cuando se cumplen las condiciones de seguridad correspondientes al modo configurado.

Un-asiento: el piloto debe tener el cinturón abrochado.

Multi-asiento: el piloto debe tener el cinturón abrochado y el número de pasajeros declarados debe coincidir con el número de cinturones abrochados.

Cuando se cumplen las condiciones, el sistema desactiva el relé y reproduce el Audio 12, indicando la habilitación del vehículo.

3.4. Fase 4 - Apagado (Contacto OFF)
Cuando se retira el contacto, el sistema no considera inmediatamente que el vehículo haya sido apagado. Realiza una confirmación mediante 200 ciclos, equivalentes aproximadamente a 10 segundos. Una vez confirmada la condición de contacto OFF, el sistema se apaga y entra en modo de bajo consumo Light Sleep.

4. Comportamiento ante eventos
Evento	Condición	Acción de control	Respuesta
Contacto ON	Vehículo detenido	PIN_RELAY = 1; inicia ciclo	Audio 15 o Audio 11 según configuración
Pulsación de botón	Relé activo / vehículo detenido	Incrementa pasajeros declarados	Audio 1 a Audio 5 según selección
Cinturón abrochado	Cualquier estado	Actualiza estado del asiento a true	LED del asiento en verde
Cinturón desabrochado	Vehículo en movimiento y desvío > umbral	Registra infracción individual y activa alerta	LED rojo titilante/fijo + audio de alerta
Re-abroche	Vehículo en movimiento y asiento en alerta	Cancela alerta e infracción del asiento	LED vuelve a verde; audio de alerta se silencia al finalizar el ciclo
Detención del vehículo	Sin movimiento	Resetea registros de infracción	Silencia audios de alerta en bucle
5. Lógica de monitoreo de cinturones
Cada asiento mantiene un estado asociado al cinturón. Cuando el cinturón se abrocha, el estado se actualiza a verdadero y, en modo multi-asiento, el indicador correspondiente pasa a verde. Si el cinturón se desabrocha mientras el vehículo está en movimiento y el desvío supera el umbral establecido, el evento se considera una infracción.

La infracción se registra individualmente para identificar el asiento que generó la condición. Mientras permanezca la condición de alerta, el sistema activa la señal visual y reproduce el aviso audible correspondiente. Si el ocupante vuelve a abrocharse el cinturón durante el movimiento, la infracción y la alerta asociadas a ese asiento se cancelan.

6. Lógica de desbloqueo
La decisión de liberar el bloqueo puede expresarse de forma lógica como:

Mono-asiento: DESBLOQUEO = Piloto_Abrochado

Multi-asiento: DESBLOQUEO = Piloto_Abrochado AND (Pasajeros_Declarados = Cinturones_Abrochados)

Por lo tanto, el relé permanece activo ante cualquier incumplimiento de las condiciones. La modificación del estado de un cinturón puede provocar una nueva evaluación de la condición de desbloqueo durante el ciclo de operación.

7. Secuencia resumida del ciclo de viaje
Contacto ON detectado.

Activación del relé de bloqueo.

Reproducción del audio de inicio.

Determinación del modo según ca.

Declaración de ocupantes mediante pulsador cuando corresponde.

Monitoreo continuo de los cinturones y del movimiento del vehículo.

Evaluación de las condiciones de seguridad.

Si las condiciones se cumplen: relé OFF + Audio 12.

Si un cinturón se desabrocha en movimiento: registrar infracción + alerta.

Si el cinturón vuelve a abrocharse: cancelar alerta e infracción.

Al detectar detención: resetear registros de infracción y silenciar alertas.

Al detectar Contacto OFF: confirmar durante aproximadamente 10 s, desactivar relé y entrar en Light Sleep.

8. Tabla de estados del sistema
Estado	Relé	Comportamiento principal
Inicio / Bloqueado	1	Vehículo bloqueado; se verifican las condiciones de seguridad.
Declaración	1	Se selecciona el número de pasajeros en modo multi-asiento.
Verificación	1	Se comprueban piloto, pasajeros declarados y cinturones.
Habilitado	0	Condiciones satisfechas; circulación permitida.
Alerta	0 o 1 según condición de bloqueo	Se informa el cinturón desabrochado mediante LED y audio; la lógica de seguridad continúa evaluándose.
Apagado	0	Contacto OFF confirmado; entrada a Light Sleep.
9. Consideraciones de comportamiento
Las infracciones se gestionan por asiento, permitiendo cancelar únicamente la condición asociada al cinturón que vuelve a abrocharse.

El estado de movimiento del vehículo es determinante para diferenciar un desabroche normal de una infracción.

La detención del vehículo permite limpiar los registros de infracción y detener las alertas repetitivas.

La configuración almacenada en NVS determina la interfaz de usuario y las condiciones de desbloqueo.

El sistema debe conservar el comportamiento definido durante todo el ciclo de viaje y reiniciar la lógica al comenzar un nuevo ciclo de contacto.

El sistema puede ser modificado a la configuración de asientos según lo requiera el cliente; existe una app que permite modificar la cantidad de asientos que se requiere, pero ello implica colocar más cableado al módulo.

La lógica de cinturones contempla el caso de que un tripulante decida bajar de la unidad cuando el coche esté detenido; su LED correspondiente estará en rojo, pero no sonará audio. Para desactivar el LED del cinturón desabrochado basta con pulsar una vez el pulsador.

Se considera que hay movimiento si se supera el umbral de 0.5 y han pasado 5 segundos. Este umbral también es configurable dependiendo de cuánto se mueve el vehículo en ruta.
