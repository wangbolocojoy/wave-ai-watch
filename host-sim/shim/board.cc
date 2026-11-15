#include "board.h"

Board& Board::GetInstance() {
    static Board board;
    return board;
}
