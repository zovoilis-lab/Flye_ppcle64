.PHONY: all clean debug profile

CXXFLAGS += -Wall -Wextra -pthread -std=c++11 -g -fsigned-char
LDFLAGS += -pthread -std=c++11 -rdynamic

MODULES_BIN := ${BIN_DIR}/flye-modules

profile: CXXFLAGS += -pg
profile: LDFLAGS += -pg
profile: release

release: CXXFLAGS += -O3 -DNDEBUG
release: flye-modules

SANITIZE_FLAGS += -D_GLIBCXX_SANITIZE_VECTOR -U_FORTIFY_SOURCE -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined
#SANITIZE_FLAGS += -fsanitize=thread -fsanitize=leak -fsanitize=undefined
debug: CXXFLAGS += -Og ${SANITIZE_FLAGS}
#debug: CXXFLAGS += -D_GLIBCXX_DEBUG
debug: LDFLAGS += ${SANITIZE_FLAGS}
debug: flye-modules


#sequence module
sequence_obj := ${patsubst %.cpp,%.o,${wildcard sequence/*.cpp}}

sequence/%.o: sequence/%.cpp sequence/*.h common/*.h
	${CXX} -c ${CXXFLAGS} $< -o $@

#flye-assemble module
assemble_obj := ${patsubst %.cpp,%.o,${wildcard assemble/*.cpp}}

assemble/%.o: assemble/%.cpp assemble/*.h sequence/*.h common/*.h
	${CXX} -c ${CXXFLAGS} $< -o $@

#flye-repeat module
repeat_obj := ${patsubst %.cpp,%.o,${wildcard repeat_graph/*.cpp}}

repeat_graph/%.o: repeat_graph/%.cpp repeat_graph/*.h sequence/*.h common/*.h
	${CXX} -c ${CXXFLAGS} $< -o $@

#flye-contigger module
contigger_obj := ${patsubst %.cpp,%.o,${wildcard contigger/*.cpp}}

contigger/%.o: contigger/%.cpp repeat_graph/*.h sequence/*.h common/*.h
	${CXX} -c ${CXXFLAGS} $< -o $@


#flye-polish module
polish_obj := ${patsubst %.cpp,%.o,${wildcard polishing/*.cpp}}

polishing/%.o: polishing/%.cpp bin/polisher.cpp polishing/*.h common/*h
	${CXX} -c ${CXXFLAGS} $< -o $@

#main module
#main_obj := ${patsubst %.cpp,%.o,${wildcard main/*.cpp}}
main_obj := main.o
flye-modules: ${assemble_obj} ${sequence_obj} ${repeat_obj} ${contigger_obj} ${polish_obj} ${main_obj}
	${CXX} ${assemble_obj} ${sequence_obj} ${repeat_obj} ${contigger_obj} ${polish_obj} ${main_obj} -o ${MODULES_BIN} ${LDFLAGS}

#main/%.o: main/%.cpp assemble/*.h sequence/*.h common/*.h repeat_graph/*.h contigger/*.h polishing/*.h
main.o: main.cpp
	${CXX} -c ${CXXFLAGS} $< -o $@


clean:
	-rm ${repeat_obj}
	-rm ${sequence_obj}
	-rm ${assemble_obj}
	-rm ${polish_obj}
	-rm ${contigger_obj}
	-rm ${main_obj}
	-rm ${MODULES_BIN}
